/*
 * KTCPVS       An implementation of the TCP Virtual Server daemon inside
 *              kernel for the LINUX operating system. KTCPVS can be used
 *              to build a moderately scalable and highly available server
 *              based on a cluster of servers, with more flexibility.
 *
 * Version:     $Id: misc.c,v 1.9 2003/05/23 02:08:34 wensong Exp $
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
#include <linux/types.h>
#include <linux/net.h>
#include <linux/skbuff.h>
//#include <linux/un.h>
#include <linux/unistd.h>
//#include <linux/wrapper.h>
#include <linux/wait.h>
#include <linux/ctype.h>
#include <asm/unistd.h>
#include <net/ip.h>
#include <net/sock.h>
#include <net/tcp.h>

#include "tcp_vs.h"


int
StartListening(struct tcp_vs_service *svc)
{
	struct socket *sock;
	struct sockaddr_in sin;
	int error;

	EnterFunction(3);

	/* First create a socket */
	error = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
	if (error < 0) {
		TCP_VS_ERR
		    ("Error during creation of socket; terminating\n");
		return error;
	}

	/* Now bind the socket */
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = svc->conf.addr;
	sin.sin_port = svc->conf.port;

	/* set the option to reuse the address. */
	sock->sk->sk_reuse = 1;

	error =
	    sock->ops->bind(sock, (struct sockaddr *) &sin, sizeof(sin));
	if (error < 0) {
		TCP_VS_ERR("Error binding socket. This means that some "
			   "other daemon is (or was a short time ago) "
			   "using %u.%u.%u.%u:%d.\n",
			   NIPQUAD(svc->conf.addr), ntohs(svc->conf.port));
		return error;
	}

	/* Now, start listening on the socket */
	error = sock->ops->listen(sock, sysctl_ktcpvs_max_backlog);
	if (error != 0) {
		TCP_VS_ERR("ktcpvs: Error listening on socket \n");
		return error;
	}

	svc->mainsock = sock;

	LeaveFunction(3);
	return 0;
}


void
StopListening(struct tcp_vs_service *svc)
{
	struct socket *sock;

	EnterFunction(3);
	if (svc->mainsock == NULL)
		return;

	sock = svc->mainsock;
	svc->mainsock = NULL;
	sock_release(sock);

	LeaveFunction(3);
}


struct socket *
tcp_vs_connect2dest(tcp_vs_dest_t * dest)
{

	struct socket *sock;
	struct sockaddr_in sin;
	int error;

	EnterFunction(5);

	/* First create a socket */
	error = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
	if (error < 0)
		TCP_VS_ERR
		    ("Error during creation of socket; terminating\n");

	/* Now connect to the destination server */
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = dest->addr;
	sin.sin_port = dest->port;

	error = sock->ops->connect(sock, (struct sockaddr *) &sin,
				   sizeof(sin), 0);
	if (error < 0) {
		TCP_VS_ERR("Error connecting to the remote host: addr=%u.%u.%u.%u port=%u\n",NIPQUAD(dest->addr),ntohs(dest->port));
		return NULL;
	}

	LeaveFunction(5);
	return sock;
}


/*
 * tcp_vs_xmit is to send bytes from the buffer to the socket.
 *
 * A positive return-value indicates the number of bytes sent, a negative
 * value indicates an error-condition.
 *
 * Note: tcp_vs_xmit will xmit all the bytes or fail.
 */
int
tcp_vs_xmit(struct socket *sock, const char *buffer,
	    const size_t length, unsigned long flags)
{
	struct msghdr msg;
	mm_segment_t oldfs;
	struct iovec iov;
	int nbytes;
	int len = 0;
	int offset = 0;
	int ret = length;

	EnterFunction(6);

	assert(buffer != NULL);

	nbytes = length;

	msg.msg_name = 0;
	msg.msg_namelen = 0;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_flags = MSG_NOSIGNAL | flags;

	while (nbytes > 0) {
		msg.msg_iov->iov_base = (void *) buffer + offset;
		msg.msg_iov->iov_len = nbytes;

		oldfs = get_fs();
		set_fs(KERNEL_DS);
		len = sock_sendmsg(sock, &msg, nbytes);
		set_fs(oldfs);

		if (len < 0) {
			ret = -1;
			break;
		}

		nbytes -= len;
		offset += len;
	}

	LeaveFunction(6);
	return ret;
}


/*
 * tcp_vs_sendbuffer is to send bytes from the buffer to the socket.
 *
 * A positive return-value indicates the number of bytes sent, a negative
 * value indicates an error-condition.
 */
int
tcp_vs_sendbuffer(struct socket *sock, const char *buffer,
		  const size_t length, unsigned long flags)
{
	struct msghdr msg;
	mm_segment_t oldfs;
	struct iovec iov;
	int len;

	EnterFunction(6);
	msg.msg_name = 0;
	msg.msg_namelen = 0;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_flags = MSG_NOSIGNAL | flags;
	msg.msg_iov->iov_base = (void *) buffer;
	msg.msg_iov->iov_len = length;

	oldfs = get_fs();
	set_fs(KERNEL_DS);
	len = sock_sendmsg(sock, &msg, length);
	set_fs(oldfs);

	LeaveFunction(6);
	return len;
}


int
tcp_vs_recvbuffer(struct socket *sock, char *buffer,
		  const size_t buflen, unsigned long flags)
{
	struct msghdr msg;
	struct iovec iov;
	int len;
	mm_segment_t oldfs;

	EnterFunction(6);

	/* Receive a packet */
	msg.msg_name = 0;
	msg.msg_namelen = 0;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_flags = 0;

	iov.iov_base = buffer;
	iov.iov_len = (size_t) buflen;

	oldfs = get_fs();
	set_fs(KERNEL_DS);
	len = sock_recvmsg(sock, &msg, buflen, flags);
	set_fs(oldfs);

	if (len < 0)
		return -1;

	LeaveFunction(6);
	return len;
}


char *
tcp_vs_getline(char *s, char *token, int n)
{
	int i;

	token[0] = '\0';

	if (s == NULL)
		return NULL;

	if (*s == '\0')
		return NULL;

	while (*s == '\n')
		s++;

	i = 0;
	while (*s != '\0' && *s != '\n') {
		if (i < n - 1) {
			token[i] = *s;
			i++;
		}
		s++;
	}

	token[i] = '\0';

	return s;
}


char *
tcp_vs_getword(char *s, char *token, int n)
{
	int i;

	token[0] = '\0';

	if (s == NULL)
		return NULL;

	while (isspace(*s))
		s++;
	if (*s == '\0' || *s == '\n')
		return NULL;

	i = 0;
	while (*s != '\0' && *s != '\n' && !isspace(*s)) {
		if (i < n - 1) {
			token[i] = *s;
			i++;
		}
		s++;
	}

	token[i] = '\0';

	return s;
}
