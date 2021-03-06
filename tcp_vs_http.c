/*
 * KTCPVS       An implementation of the TCP Virtual Server daemon inside
 *              kernel for the LINUX operating system. KTCPVS can be used
 *              to build a moderately scalable and highly available server
 *              based on a cluster of servers, with more flexibility.
 *
 * tcp_vs_http.c: KTCPVS content-based scheduling module for HTTP service
 *
 * Version:     $Id: tcp_vs_http.c,v 1.13 2003/06/14 16:09:47 wensong Exp $
 *
 * Authors:     Wensong Zhang <wensong@linuxvirtualserver.org>
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

#include <linux/net.h>
#include <linux/sched.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <asm/uaccess.h>

#include "tcp_vs.h"
#include "tcp_vs_http_parser.h"


static int
tcp_vs_http_init_svc(struct tcp_vs_service *svc)
{
	return 0;
}


static int
tcp_vs_http_done_svc(struct tcp_vs_service *svc)
{
	return 0;
}


static int
tcp_vs_http_update_svc(struct tcp_vs_service *svc)
{
	return 0;
}


static inline tcp_vs_dest_t *
__tcp_vs_http_wlc_schedule(struct list_head *destinations)
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
	 *    Find the destination with the least load.
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


typedef enum http_methods {
	METHOD_NONE,
	METHOD_GET,
	METHOD_HEAD,
	METHOD_POST,
	METHOD_PUT,
	NR_METHODS
} http_method_t;

typedef enum http_versions {
	NOTKNOWN,
	HTTP_1_0,
	HTTP_1_1
} http_version_t;

#define GOTO_INCOMPLETE							\
do {									\
	TCP_VS_DBG(5, "incomplete at %s:%d.\n", __FILE__, __LINE__);	\
	goto incomplete_message;					\
} while (0)

#define GOTO_ERROR							\
do {									\
	TCP_VS_DBG(5, "parse error at %s:%d.\n", __FILE__, __LINE__);	\
	goto error;							\
} while (0)

#define get_c(ptr,eob)				\
({						\
	if (ptr >= eob)				\
		GOTO_INCOMPLETE;		\
	*((ptr)++);				\
})

#define PARSE_TOKEN(ptr,str,eob)		\
({						\
	int __ret;				\
						\
	if (ptr+sizeof(str)-1 > eob) {		\
		GOTO_INCOMPLETE;		\
	}					\
						\
	if (memcmp(ptr, str, sizeof(str)-1))	\
		__ret = 0;			\
	else {					\
		ptr += sizeof(str)-1;		\
		__ret = 1;			\
	}					\
	__ret;					\
})


/*
 *      Parse HTTP header
 */
int
parse_http_header(const char *buffer, size_t buflen, http_request_t * req)
{
	const char *curr, *eob;
	char c;

	EnterFunction(5);

	TCP_VS_DBG(5, "parsing request:\n");
	TCP_VS_DBG(5, "--------------------\n");
	TCP_VS_DBG(5, "%s\n", buffer);
	TCP_VS_DBG(5, "--------------------\n");

	/* parse only the first header if multiple headers are present */

	curr = buffer;
	eob = buffer + buflen;

	req->message = buffer;
	req->message_len = buflen;

	/*
	 * RFC 2616, 5.1:
	 *      Request-Line = Method SP Request-URI SP HTTP-Version CRLF
	 */
	switch (get_c(curr, eob)) {
	case 'G':
		if (PARSE_TOKEN(curr, "ET ", eob)) {
			req->method = METHOD_GET;
			break;
		}
		GOTO_ERROR;

	case 'H':
		if (PARSE_TOKEN(curr, "EAD ", eob)) {
			req->method = METHOD_HEAD;
			break;
		}
		GOTO_ERROR;

	case 'P':
		if (PARSE_TOKEN(curr, "OST ", eob)) {
			req->method = METHOD_POST;
			break;
		}
		if (PARSE_TOKEN(curr, "UT ", eob)) {
			req->method = METHOD_PUT;
			break;
		}
		GOTO_ERROR;

	default:
		GOTO_ERROR;
	}
	req->method_str = buffer;
	req->method_len = curr - buffer - 1;

	req->uri_str = curr;
	while (1) {
		c = get_c(curr, eob);
		if (isspace(c))
			break;
	}
	req->uri_len = curr - req->uri_str - 1;

	req->version_str = curr;
	if (PARSE_TOKEN(curr, "HTTP/1.", eob)) {
		switch (get_c(curr, eob)) {
		case '0':
			req->version = HTTP_1_0;
			break;
		case '1':
			req->version = HTTP_1_1;
			break;
		default:
			GOTO_ERROR;
		}
	} else
		GOTO_ERROR;

	LeaveFunction(5);
	return PARSE_OK;

      incomplete_message:
	LeaveFunction(5);
	return PARSE_INCOMPLETE;

      error:
	LeaveFunction(5);
	return PARSE_ERROR;
}


static tcp_vs_dest_t *
tcp_vs_http_matchrule(struct tcp_vs_service *svc, http_request_t * req)
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
			    __tcp_vs_http_wlc_schedule(&r->destinations);
			break;
		}
	}
	read_unlock(&svc->lock);

	kfree(uri);
	return dest;
}


/*
 *    HTTP content-based scheduling
 *    Parse the http request, select a server according to the
 *    request, and create a socket the server finally.
 */
static int
tcp_vs_http_schedule(struct tcp_vs_conn *conn, struct tcp_vs_service *svc)
{
	tcp_vs_dest_t *dest;
	struct socket *csock, *dsock;
	char *buffer;
	size_t buflen;
	int len;
	http_request_t req;

	EnterFunction(5);

	buffer = conn->buffer;
	buflen = conn->buflen;
	csock = conn->csock;

	/* Do we have data ? */
	while (skb_queue_empty(&(csock->sk->sk_receive_queue))) {
		interruptible_sleep_on_timeout(&csock->wait, HZ);
	}

	/* fixme: what if the request overlap this receiving buffer */
	len = tcp_vs_recvbuffer(csock, buffer, buflen - 1, MSG_PEEK);
	if (len < 0) {
		TCP_VS_ERR_RL("error reading request from client\n");
		return -1;
	} else if (len == 0) {
		/* some clients may connect and disconnect immediately */
		return 0;
	}

	/* make it zero-terminated string */
	buffer[len] = '\0';

	if (parse_http_header(buffer, len, &req) != PARSE_OK) {
		TCP_VS_ERR_RL("cannot parse http request\n");

		/* should redirect it to a local port next time */
		return -1;
	}

	/*  Head.RemoteHost.s_addr = sock->sk->daddr; */

	dest = tcp_vs_http_matchrule(svc, &req);
	if (!dest)
		return -1;

	TCP_VS_DBG(5, "HTTP: server %d.%d.%d.%d:%d "
		   "conns %d refcnt %d weight %d\n",
		   NIPQUAD(dest->addr), ntohs(dest->port),
		   atomic_read(&dest->conns),
		   atomic_read(&dest->refcnt), dest->weight);

	dsock = tcp_vs_connect2dest(dest);
	if (!dsock) {
		TCP_VS_ERR_RL("The destination is not available\n");
		return -1;
	}

	atomic_inc(&dest->conns);
	conn->dest = dest;
	conn->dsock = dsock;

	if (tcp_vs_sendbuffer(dsock, buffer, len, 0) != len) {
		TCP_VS_ERR_RL("Error HTTP sending buffer\n");
	}

	LeaveFunction(5);

	return 0;
}


static struct tcp_vs_scheduler tcp_vs_http_scheduler = {
	{0},			/* n_list */
	"http",			/* name */
	THIS_MODULE,		/* this module */
	tcp_vs_http_init_svc,	/* initializer */
	tcp_vs_http_done_svc,	/* done */
	tcp_vs_http_update_svc,	/* update */
	tcp_vs_http_schedule,	/* select a server by http request */
};


static int __init
tcp_vs_http_init(void)
{
	INIT_LIST_HEAD(&tcp_vs_http_scheduler.n_list);
	return register_tcp_vs_scheduler(&tcp_vs_http_scheduler);
}

static void __exit
tcp_vs_http_cleanup(void)
{
	unregister_tcp_vs_scheduler(&tcp_vs_http_scheduler);
}

module_init(tcp_vs_http_init);
module_exit(tcp_vs_http_cleanup);
MODULE_LICENSE("GPL");
