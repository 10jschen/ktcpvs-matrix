/*
 * KTCPVS       An implementation of the TCP Virtual Server daemon inside
 *              kernel for the LINUX operating system. KTCPVS can be used
 *              to build a moderately scalable and highly available server
 *              based on a cluster of servers, with more flexibility.
 *
 * tcp_vs_phttp.c: KTCPVS content-based scheduling module for HTTP service
 *		   with persistent connection support.
 *
 * Version:     $Id: tcp_vs_phttp.c,v 1.13 2003/07/08 02:09:13 wensong Exp $
 *
 * Authors:     Wensong Zhang <wensong@linuxvirtualserver.org>
 *              Hai Long <david_lung@yahoo.com>
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/ctype.h>

#include <linux/skbuff.h>
#include <net/sock.h>

#include <linux/gfp.h>
#include <linux/tcp.h>

#include "tcp_vs.h"
#include "tcp_vs_http_parser.h"
#include "tcp_vs_http_trans.h"


static int
tcp_vs_phttp_init_svc(struct tcp_vs_service *svc)
{
	return 0;
}


static int
tcp_vs_phttp_done_svc(struct tcp_vs_service *svc)
{
	return 0;
}


static int
tcp_vs_phttp_update_svc(struct tcp_vs_service *svc)
{
	return 0;
}


static inline tcp_vs_dest_t *
__tcp_vs_phttp_wlc_schedule(struct list_head *destinations)
{
	register struct list_head *e;
	tcp_vs_dest_t *dest, *least;

	list_for_each(e, destinations) {
		least = list_entry(e, tcp_vs_dest_t, r_list);
		if (least->weight > 0) {
			goto nextstage;
		}
	}
	return NULL;

	/*
	 *      Find the destination with the least load.
	 */
      nextstage:
	for (e = e->next; e != destinations; e = e->next) {
		dest = list_entry(e, tcp_vs_dest_t, r_list);
		if (atomic_read(&least->conns) * dest->weight >
		    atomic_read(&dest->conns) * least->weight) {
			least = dest;
		}
	}

	return least;
}


static tcp_vs_dest_t *
tcp_vs_hhttp_matchrule(struct tcp_vs_service *svc, http_request_t * req)
{
	struct list_head *l;
	struct tcp_vs_rule *r;
	tcp_vs_dest_t *dest = NULL;
	char *uri;
	regmatch_t matches[10];
	int hashvalue, num_dest;
	int reg_err;
	regoff_t start, end, p;
	register struct list_head *e;

	if (!(uri = kmalloc(req->uri_len + 1, GFP_KERNEL))) {
		TCP_VS_ERR("No memory!\n");
		return NULL;
	}
	memcpy(uri, req->uri_str, req->uri_len);
	uri[req->uri_len] = '\0';
	TCP_VS_DBG(5, "matching request URI: %s\n", uri);

	read_lock(&svc->lock);
	list_for_each(l, &svc->rule_list) {
		r = list_entry(l, struct tcp_vs_rule, list);
		memset(matches, 0, sizeof(regmatch_t) * 10); /* initialise the values */
		reg_err = regexec(&r->rx, uri, 10, matches, 0); 
		if (!reg_err) {
			/* HIT */
			TCP_VS_DBG(5, "URI matched pattern %s\n", r->pattern);
			start = matches[r->match_num].rm_so;
			end = matches[r->match_num].rm_eo;
			if (start && end) {
				num_dest = 0;
				list_for_each(e, &r->destinations) {
					num_dest++;
				}
				hashvalue = 0;
				for (p = start; p < end; p++) {
					hashvalue += uri[p];
				}
				TCP_VS_DBG(5, "hash value %d (c=%d)\n", hashvalue, num_dest);
				if (num_dest) {
					hashvalue %= num_dest;
					list_for_each(e, &r->destinations) {
						if (hashvalue == 0) {
							dest = list_entry(e, tcp_vs_dest_t,  r_list);
							goto found;
						}
						hashvalue--;
					}
				}
			}
			break;
		} else {
			TCP_VS_DBG(6,"regexec return code is <%d>\n", reg_err);
		}
	}
  found:
	read_unlock(&svc->lock);

	kfree(uri);
	return dest;
}

static tcp_vs_dest_t *
tcp_vs_phttp_matchrule(struct tcp_vs_service *svc, http_request_t * req)
{
	struct list_head *l;
	struct tcp_vs_rule *r;
	tcp_vs_dest_t *dest = NULL;
	char *uri;

	if (!(uri = kmalloc(req->uri_len + 1, GFP_KERNEL))) {
		TCP_VS_ERR("No memory!\n");
		return NULL;
	}
	memcpy(uri, req->uri_str, req->uri_len);
	uri[req->uri_len] = '\0';
	TCP_VS_DBG(5, "matching request URI: %s\n", uri);

	read_lock(&svc->lock);
	list_for_each(l, &svc->rule_list) {
		r = list_entry(l, struct tcp_vs_rule, list);
		if (!regexec(&r->rx, uri, 0, NULL, 0)) {
			/* HIT */
			dest =
			    __tcp_vs_phttp_wlc_schedule(&r->destinations);
			break;
		}
	}
	read_unlock(&svc->lock);

	kfree(uri);
	return dest;
}

/****************************************************************************
*	get response from the specified server
*/
static int
http_get_response(struct socket *csock, struct socket *dsock,
		  http_request_t * req, char *buffer, int buflen,
		  int *close)
{
	http_read_ctl_block_t read_ctl_blk;
	http_buf_t	buff;
	http_response_t resp;
	int len, ret = -1;

	EnterFunction(5);

	INIT_LIST_HEAD(&buff.b_list);
	buff.buf = buffer;
	buff.data_len = 0;

	memset(&read_ctl_blk, 0, sizeof(read_ctl_blk));
	INIT_LIST_HEAD(&read_ctl_blk.buf_entry_list);
	read_ctl_blk.cur_buf  = &buff;
	read_ctl_blk.buf_size = buflen;
	read_ctl_blk.sock = dsock;
	list_add (&buff.b_list, &read_ctl_blk.buf_entry_list);

	*close = 0;

	/* Do we have data ? */
	while (skb_queue_empty(&(dsock->sk->sk_receive_queue))) {

		// try to detect closed connections, added by wenming
		if (dsock->sk->sk_state != TCP_ESTABLISHED) {
			goto exit;
		}
		interruptible_sleep_on_timeout(&dsock->wait, HZ);
	}

	/* read status line from server */
	len = http_read_line(&read_ctl_blk, 0);
	if (len < 0) {
		TCP_VS_ERR("Error in reading status line from server\n");
		goto exit;
	}

	/* xmit status line to client (2 more bytes for CRLF) */
	if (tcp_vs_xmit(csock, read_ctl_blk.info, len + 2, MSG_MORE) < 0) {
		TCP_VS_ERR("Error in sending status line\n");
		goto exit;
	}

	/* parse status line */
	memset(&resp, 0, sizeof(resp));
	if (parse_http_status_line(read_ctl_blk.info, len, &resp) ==
	    PARSE_ERROR) {
		goto exit;
	}

	/* parse MIME header */
	do {
		if ((len = http_read_line(&read_ctl_blk, 0)) < 0) {
			goto exit;
		}

		/* xmit MIME header (2 more bytes for CRLF) */
		if (tcp_vs_xmit
		    (csock, read_ctl_blk.info, len + 2, MSG_MORE) < 0) {
			TCP_VS_ERR("Error in sending status line\n");
			goto exit;
		}

		/* http_line_unescape (read_ctl_blk.info, len); */
		http_mime_parse(read_ctl_blk.info, len, &resp.mime);
	} while (len != 0);	/* http header end with CRLF,CRLF */

	*close = resp.mime.connection_close;

	/*
	 * Any response message which "MUST NOT" include a message-body (such
	 * as the 1xx, 204, and 304 responses and any response to a HEAD
	 * request) is always terminated by the first empty line after the
	 * header fields, regardless of the entity-header fields present in
	 * the message.
	 */
	if (req->method != HTTP_M_HEAD) {
		if ((resp.status_code < 200)
		    || (resp.status_code == 204
			|| resp.status_code == 304)) {
			ret = 0;
			goto exit;
		}

		ret =
		    relay_http_message_body(csock, &read_ctl_blk,
					    &resp.mime);
		if (resp.mime.sep) {
			kfree(resp.mime.sep);
		}
	}

      exit:
	LeaveFunction(5);
	return ret;
}


/****************************************************************************
*	HTTP content-based scheduling:
*	1, For http 1.0 request, parse the http request, select a server
*	according to the request, and create a socket the server finally.
*	2, For http 1.1 request, do all the work by itself. Parse every http
*	message header and direct each message to the right server according
*	to the scheduling rule. For a worker thread to get response by order
*	from the server.
*	returns:
*		0,	success, schedule just chose a dest server
*		1,	success, schedule has done all the jobs
*		-1,	redirect to the local server
*		-2,	error
*/
static int
tcp_vs_phttp_schedule(struct tcp_vs_conn *conn, struct tcp_vs_service *svc)
{
	http_request_t req;
	http_read_ctl_block_t read_ctl_blk;
	char *buffer = NULL;	/* store data from server */
	int ret = 1;		/* scheduler has done all the jobs */
	int len;
	unsigned long last_read;
	int close_server = 0;
	tcp_vs_dest_t *dest;
	struct socket *dsock;
	server_conn_t *sc;

	DECLARE_WAITQUEUE(wait, current);

	EnterFunction(5);

	/* init buffer for http message header */
	if (http_read_init(&read_ctl_blk, conn->csock) != 0) {
		TCP_VS_ERR("Out of memory!\n");
		ret = -2;
		goto out_nobuffer;
	}
	read_ctl_blk.flag = MSG_PEEK;

	conn->dest = NULL;
	conn->dsock = NULL;

	/* Do we have data ? */
	while (skb_queue_empty(&(conn->csock->sk->sk_receive_queue))) {
		interruptible_sleep_on_timeout(&conn->csock->wait, HZ);
	}

	/* allocate buffer to store data that get from servers */
	buffer = (char *) __get_free_page(GFP_KERNEL);
	if (buffer == NULL) {
		ret = -2;
		//goto out;
		goto out_nobuffer;
	}

	last_read = jiffies;
	do {
		switch (data_available(&read_ctl_blk)) {
		case -1:
			TCP_VS_DBG(5, "Socket error before reading "
				   "request line.\n");
			ret = -2;
			goto out;

		case 0:
			/* check if the service is stopped or system is
			   unloaded */
			if (svc->stop != 0 || sysctl_ktcpvs_unload != 0) {
				TCP_VS_DBG(5,
					   "phttp scheduling exit (pid=%d)\n",
					   current->pid);
				goto out;
			}

			if ((jiffies - last_read) >
			    sysctl_ktcpvs_keepalive_timeout * HZ) {
				TCP_VS_DBG(5, "Timeout, disconnect.\n");
				goto out;
			}

			add_wait_queue(read_ctl_blk.sock->sk->sk_sleep,
				       &wait);
			__set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ);
			__set_current_state(TASK_RUNNING);
			remove_wait_queue(read_ctl_blk.sock->sk->sk_sleep,
					  &wait);
			continue;

		case 1:
			last_read = jiffies;
			break;
		}

		/* read request line from client socket */
		len = http_read_line(&read_ctl_blk, 0);
		if (len < 0) {
			TCP_VS_ERR("Error reading request line from client\n");
			ret = -2;
			goto out;
		}

		/* parse the http request line */
		memset(&req, 0, sizeof(req));
		if (parse_http_request_line(read_ctl_blk.info, len, &req)
		    != PARSE_OK) {
			TCP_VS_ERR("Cannot parse http request\n");
			ret = -2;
			goto out;
		}

		/* select a server */
		//dest = tcp_vs_phttp_matchrule(svc, &req);
		/* enable load balance */
		dest = tcp_vs_hhttp_matchrule(svc, &req);
		if (!dest) {
			TCP_VS_DBG(5, "Can't find a right server\n");
			if (read_ctl_blk.flag == MSG_PEEK) {
				/* redirect to a local port */
				ret = -1;
			} else {
				ret = -2;
			}
			goto out;
		}

		if (req.version <= HTTP_VERSION(1, 0)) {
			TCP_VS_DBG(5, "HTTP: server %d.%d.%d.%d:%d "
				   "conns %d refcnt %d weight %d active %d\n",
				   NIPQUAD(dest->addr), ntohs(dest->port),
				   atomic_read(&dest->conns),
				   atomic_read(&dest->refcnt),
				   dest->weight,dest->active);

			conn->dsock = tcp_vs_connect2dest(dest);
			if (conn->dsock == NULL) {
				TCP_VS_ERR
				    ("The destination is not available!\n");
				goto out;
			}

			atomic_inc(&dest->conns);
			conn->dest = dest;
			ret = 0;
			goto out;
		}


		/*
		 * For http 1.1 client, continue processing for
		 * persistent connections
		 */

		/* lookup a server connection in the connection pool */
	      lookup_again:
		sc = tcp_vs_srvconn_get(dest->addr, dest->port);
		if (sc == NULL) {
			sc = tcp_vs_srvconn_new(dest);
			if (sc == NULL) {
				/* mark it's not active */
				dest->active = 0;
				if (read_ctl_blk.flag == MSG_PEEK) {
					/* redirect to a local port */
					ret = -1;
				} else {
					ret = -2;
				}
				goto out;
			}
			/* mark it's active */
			dest->active = 1;
		}
		dsock = sc->sock;
		if (dsock->sk->sk_state != TCP_ESTABLISHED) {
			tcp_vs_srvconn_free(sc);
			goto lookup_again;
		}

		/* re-read the peeked data for the first http request of
		   a connection */
		if (read_ctl_blk.flag == MSG_PEEK) {
			read_ctl_blk.flag = 0;
			read_ctl_blk.offset = 0;
			read_ctl_blk.remaining = 0;
			if (tcp_vs_recvbuffer(conn->csock,
					      read_ctl_blk.cur_buf->buf,
					      len + 2, 0) != (len + 2)) {
				TCP_VS_ERR("Error in re-reading http "
					   "request line\n");
				tcp_vs_srvconn_put(sc);
				goto out;
			}
		}

		/* xmit request line (2 more bytes for CRLF) */
		if (tcp_vs_xmit
		    (dsock, read_ctl_blk.info, len + 2, MSG_MORE) < 0) {
			TCP_VS_ERR("Error in sending request line\n");
			goto out_free;
		}

		/* Process MIME header */
		do {
			len = http_read_line(&read_ctl_blk, 0);
			if (len < 0) {
				tcp_vs_srvconn_put(sc);
				goto out;
			}

			/* xmit MIME header (2 more bytes for CRLF) */
			if (tcp_vs_xmit
			    (dsock, read_ctl_blk.info, len + 2,
			     MSG_MORE) < 0) {
				TCP_VS_ERR("Error in sending MIME header\n");
				goto out_free;
			}

			/* http_line_unescape (read_ctl_blk.info, len); */
			http_mime_parse(read_ctl_blk.info, len, &req.mime);
		} while (len != 0);	/* http header end with CRLF,CRLF */

		if (relay_http_message_body
		    (dsock, &read_ctl_blk, &req.mime) != 0) {
			TCP_VS_ERR("Error in sending http message body\n");
			goto out_free;
		}

		if (http_get_response(conn->csock, dsock, &req, buffer,
				      PAGE_SIZE, &close_server) < 0) {
			goto out_free;
		}

		if (close_server) {
			TCP_VS_DBG(5, "Close server connection.\n");
			goto out_free;	/* close the connection? tbd */
		}

		/* have to put back the server connection when
		   it is not used */
		tcp_vs_srvconn_put(sc);
	} while (req.mime.connection_close != 1 && conn->csock->sk->sk_state == TCP_ESTABLISHED);

      out:
	free_page((unsigned long) buffer);
      out_nobuffer:
	http_read_free(&read_ctl_blk);
	LeaveFunction(5);
	return ret;

      out_free:
	tcp_vs_srvconn_free(sc);
	goto out;
}

static struct tcp_vs_scheduler tcp_vs_phttp_scheduler = {
	{0},			/* n_list */
	"phttp",		/* name */
	THIS_MODULE,		/* this module */
	tcp_vs_phttp_init_svc,	/* initializer */
	tcp_vs_phttp_done_svc,	/* done */
	tcp_vs_phttp_update_svc,	/* update */
	tcp_vs_phttp_schedule,	/* select a server by http request */
};

static int __init
tcp_vs_phttp_init(void)
{
	http_mime_parser_init();
	INIT_LIST_HEAD(&tcp_vs_phttp_scheduler.n_list);
	return register_tcp_vs_scheduler(&tcp_vs_phttp_scheduler);
}

static void __exit
tcp_vs_phttp_cleanup(void)
{
	unregister_tcp_vs_scheduler(&tcp_vs_phttp_scheduler);
}

module_init(tcp_vs_phttp_init);
module_exit(tcp_vs_phttp_cleanup);
MODULE_LICENSE("GPL");
