/*
 * libtcpvs:	Library for manipulating TCPVS through [gs]etsockopt
 *
 * Version:     $Id: libtcpvs.c,v 1.4 2003/05/23 02:08:34 wensong Exp $
 *
 * Authors:     Wensong Zhang <wensong@linuxvirtualserver.org>
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "libtcpvs/libtcpvs.h"

static int sockfd = -1;
static int tcpvs_cmd = 0;
struct tcp_vs_getinfo tcpvs_info;


int
tcpvs_init(void)
{
	socklen_t len;

	len = sizeof(tcpvs_info);
	if ((sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) == -1)
		return -1;

	if (getsockopt(sockfd, IPPROTO_IP, TCP_VS_SO_GET_INFO,
		       (char *) &tcpvs_info, &len))
		return -1;

	return 0;
}


unsigned int
tcpvs_version(void)
{
	return tcpvs_info.version;
}


static inline void *
memdup2(const void *src1, size_t n1, const void *src2, size_t n2)
{
	void *dest;
	if (!(dest = malloc(n1 + n2)))
		return NULL;

	memcpy(dest, src1, n1);
	memcpy(dest + n1, src2, n2);
	return dest;
}


int
tcpvs_add_service(struct tcp_vs_ident *id, struct tcp_vs_config *conf)
{
	void *arg;
	int result;

	if (!(arg = memdup2(id, sizeof(*id), conf, sizeof(*conf))))
		return ENOMEM;

	tcpvs_cmd = TCP_VS_SO_SET_ADD;
	result = setsockopt(sockfd, IPPROTO_IP, TCP_VS_SO_SET_ADD,
			    arg, sizeof(*id) + sizeof(*conf));
	free(arg);
	return result;
}

int
tcpvs_edit_service(struct tcp_vs_ident *id, struct tcp_vs_config *conf)
{
	void *arg;
	int result;

	if (!(arg = memdup2(id, sizeof(*id), conf, sizeof(*conf))))
		return ENOMEM;

	result = setsockopt(sockfd, IPPROTO_IP, TCP_VS_SO_SET_EDIT,
			    arg, sizeof(*id) + sizeof(*conf));
	free(arg);
	return result;
}


int
tcpvs_del_service(struct tcp_vs_ident *id)
{
	tcpvs_cmd = TCP_VS_SO_SET_DEL;
	return setsockopt(sockfd, IPPROTO_IP,
			  TCP_VS_SO_SET_DEL, (void *) id, sizeof(*id));
}


int
tcpvs_start_service(struct tcp_vs_ident *id)
{
	tcpvs_cmd = TCP_VS_SO_SET_START;
	return setsockopt(sockfd, IPPROTO_IP,
			  TCP_VS_SO_SET_START, (void *) id, sizeof(*id));
}


int
tcpvs_stop_service(struct tcp_vs_ident *id)
{
	tcpvs_cmd = TCP_VS_SO_SET_STOP;
	return setsockopt(sockfd, IPPROTO_IP,
			  TCP_VS_SO_SET_STOP, (void *) id, sizeof(*id));
}


int
tcpvs_flush(void)
{
	tcpvs_cmd = TCP_VS_SO_SET_FLUSH;
	return setsockopt(sockfd, IPPROTO_IP,
			  TCP_VS_SO_SET_FLUSH, NULL, 0);
}

int
tcpvs_add_dest(struct tcp_vs_ident *id, struct tcp_vs_dest_u *dest)
{
	void *arg;
	int result;

	if (!(arg = memdup2(id, sizeof(*id), dest, sizeof(*dest))))
		return ENOMEM;

	tcpvs_cmd = TCP_VS_SO_SET_ADDDEST;
	result = setsockopt(sockfd, IPPROTO_IP, TCP_VS_SO_SET_ADDDEST,
			    arg, sizeof(*id) + sizeof(*dest));
	free(arg);
	return result;
}

int
tcpvs_edit_dest(struct tcp_vs_ident *id, struct tcp_vs_dest_u *dest)
{
	void *arg;
	int result;

	if (!(arg = memdup2(id, sizeof(*id), dest, sizeof(*dest))))
		return ENOMEM;

	tcpvs_cmd = TCP_VS_SO_SET_EDITDEST;
	result = setsockopt(sockfd, IPPROTO_IP, TCP_VS_SO_SET_EDITDEST,
			    arg, sizeof(*id) + sizeof(*dest));
	free(arg);
	return result;
}

int
tcpvs_del_dest(struct tcp_vs_ident *id, struct tcp_vs_dest_u *dest)
{
	void *arg;
	int result;

	if (!(arg = memdup2(id, sizeof(*id), dest, sizeof(*dest))))
		return ENOMEM;

	tcpvs_cmd = TCP_VS_SO_SET_DELDEST;
	result = setsockopt(sockfd, IPPROTO_IP, TCP_VS_SO_SET_DELDEST,
			    arg, sizeof(*id) + sizeof(*dest));
	free(arg);
	return result;
}


int
tcpvs_add_rule(struct tcp_vs_ident *id, struct tcp_vs_rule_u *rule)
{
	void *arg;
	int result;

	if (!(arg = memdup2(id, sizeof(*id), rule, sizeof(*rule))))
		return ENOMEM;

	tcpvs_cmd = TCP_VS_SO_SET_ADDRULE;
	result = setsockopt(sockfd, IPPROTO_IP, TCP_VS_SO_SET_ADDRULE,
			    arg, sizeof(*id) + sizeof(*rule));
	free(arg);
	return result;
}


int
tcpvs_del_rule(struct tcp_vs_ident *id, struct tcp_vs_rule_u *rule)
{
	void *arg;
	int result;

	if (!(arg = memdup2(id, sizeof(*id), rule, sizeof(*rule))))
		return ENOMEM;

	tcpvs_cmd = TCP_VS_SO_SET_DELRULE;
	result = setsockopt(sockfd, IPPROTO_IP, TCP_VS_SO_SET_DELRULE,
			    arg, sizeof(*id) + sizeof(*rule));
	free(arg);
	return result;
}


struct tcp_vs_service_u *
tcpvs_get_service(struct tcp_vs_ident *id)
{
	struct tcp_vs_service_u *svc;
	socklen_t len;

	len = sizeof(*svc);
	if (!(svc = malloc(len)))
		return NULL;

	strcpy(svc->ident.name, id->name);
	if (getsockopt(sockfd, IPPROTO_IP, TCP_VS_SO_GET_SERVICE,
		       (char *) svc, &len)) {
		free(svc);
		return NULL;
	}
	return svc;
}


struct tcp_vs_get_services *
tcpvs_get_services(void)
{
	struct tcp_vs_get_services *get;
	socklen_t len;

	len = sizeof(*get) +
	    sizeof(struct tcp_vs_service_u) * tcpvs_info.num_services;
	if (!(get = malloc(len)))
		return NULL;

	get->num_services = tcpvs_info.num_services;
	if (getsockopt(sockfd, IPPROTO_IP,
		       TCP_VS_SO_GET_SERVICES, get, &len) < 0) {
		free(get);
		return NULL;
	}
	return get;
}


struct tcp_vs_get_dests *
tcpvs_get_dests(struct tcp_vs_service_u *svc)
{
	struct tcp_vs_get_dests *d;
	socklen_t len;

	len = sizeof(*d) + sizeof(struct tcp_vs_dest_u) * svc->num_dests;
	if (!(d = malloc(len)))
		return NULL;

	strcpy(d->ident.name, svc->ident.name);
	d->num_dests = svc->num_dests;

	if (getsockopt(sockfd, IPPROTO_IP,
		       TCP_VS_SO_GET_DESTS, d, &len) < 0) {
		free(d);
		return NULL;
	}
	return d;
}


struct tcp_vs_get_rules *
tcpvs_get_rules(struct tcp_vs_service_u *svc)
{
	struct tcp_vs_get_rules *r;
	socklen_t len;

	len = sizeof(*r) + sizeof(struct tcp_vs_rule_u) * svc->num_rules;
	if (!(r = malloc(len)))
		return NULL;

	strcpy(r->ident.name, svc->ident.name);
	r->num_rules = svc->num_rules;

	if (getsockopt(sockfd, IPPROTO_IP,
		       TCP_VS_SO_GET_RULES, r, &len) < 0) {
		free(r);
		return NULL;
	}
	return r;
}


void
tcpvs_close(void)
{
	close(sockfd);
}


const char *
tcpvs_strerror(int err)
{
	unsigned int i;
	struct table_struct {
		int cmd;
		int err;
		const char *message;
	} table[] = { {
	0, EPERM, "Permission denied (you must be root)"}, {
	0, EINVAL, "Module is wrong version"}, {
	0, ENOPROTOOPT, "The ktcpvs module not loaded"}, {
	0, ENOMEM, "Memory allocation problem"}, {
	TCP_VS_SO_SET_ADD, EEXIST, "Service already exists"}, {
	TCP_VS_SO_SET_ADD, ENOENT, "Scheduler not found"}, {
	TCP_VS_SO_SET_EDIT, ESRCH, "No such service"}, {
	TCP_VS_SO_SET_EDIT, ENOENT, "Scheduler not found"}, {
	TCP_VS_SO_SET_DEL, ESRCH, "No such service"}, {
	TCP_VS_SO_SET_ADDDEST, ESRCH, "Service not defined"}, {
	TCP_VS_SO_SET_ADDDEST, EEXIST, "Destination already exists"}, {
	TCP_VS_SO_SET_EDITDEST, ESRCH, "Service not defined"}, {
	TCP_VS_SO_SET_EDITDEST, ENOENT, "No such destination"}, {
	TCP_VS_SO_SET_DELDEST, ESRCH, "Service not defined"}, {
	TCP_VS_SO_SET_DELDEST, ENOENT, "No such destination"}, {
	TCP_VS_SO_SET_DELDEST, EBUSY, "Destination is busy"},};

	for (i = 0; i < sizeof(table) / sizeof(struct table_struct); i++) {
		if ((!table[i].cmd || table[i].cmd == tcpvs_cmd)
		    && table[i].err == err)
			return table[i].message;
	}

	return strerror(err);
}
