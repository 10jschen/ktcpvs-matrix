/*
 * KTCPVS       An implementation of the TCP Virtual Server daemon inside
 *              kernel for the LINUX operating system. KTCPVS can be used
 *              to build a moderately scalable and highly available server
 *              based on a cluster of servers, with more flexibility.
 *
 * tcp_vs_srvconn.c: server connection management for KTCPVS
 *
 * Version:     $Id: tcp_vs_srvconn.c,v 1.5 2003/05/23 02:51:12 wensong Exp $
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


/*
 *	KTCPVS server connections flags
 */
#define SRVCONN_F_NONE		0x0000
#define SRVCONN_F_HASHED	0x0001

/*
 *  Connection hash table: for connections connected to the servers
 */
#define SRVCONN_TAB_BITS	8
#define SRVCONN_TAB_SIZE	(1 << SRVCONN_TAB_BITS)
#define SRVCONN_TAB_MASK	(SRVCONN_TAB_SIZE - 1)

static struct list_head srvconn_tab[SRVCONN_TAB_SIZE];

/*  SLAB cache for server connections */
static kmem_cache_t *srvconn_cachep;

/* lock for the table of server connections */
static spinlock_t srvconn_lock = SPIN_LOCK_UNLOCKED;

/*  counter for current server connections */
static atomic_t srvconn_counter = ATOMIC_INIT(0);


/*
 *	Returns hash value for the server connection entry
 */
static inline unsigned
tcp_vs_srvconn_hashkey(__u32 addr, __u16 port)
{
	unsigned addrh = ntohl(addr);

	return (addrh ^ (addrh >> SRVCONN_TAB_BITS) ^ ntohs(port))
	    & SRVCONN_TAB_MASK;
}


/*
 *	Hashes a connection into the server connection table.
 *	returns bool success.
 */
static inline int
tcp_vs_srvconn_hash(server_conn_t * sc)
{
	unsigned hash;

	if (sc->flags & SRVCONN_F_HASHED) {
		TCP_VS_ERR
		    ("tcp_vs_srvconn_hash: request for already hashed, "
		     "called from %p\n", __builtin_return_address(0));
		return 0;
	}

	/* Hash by <address, port> */
	hash = tcp_vs_srvconn_hashkey(sc->addr, sc->port);

	spin_lock(&srvconn_lock);

	list_add(&sc->list, &srvconn_tab[hash]);
	sc->flags |= SRVCONN_F_HASHED;

	spin_unlock(&srvconn_lock);

	return 1;
}


/*
 *	UNhashes a connection from the server connection table.
 *	returns bool success.
 */
static inline void
__tcp_vs_srvconn_unhash(server_conn_t * sc)
{
	list_del(&sc->list);
	sc->flags &= ~SRVCONN_F_HASHED;
}

static inline int
tcp_vs_srvconn_unhash(server_conn_t * sc)
{
	if (!(sc->flags & SRVCONN_F_HASHED)) {
		TCP_VS_ERR
		    ("tcp_vs_srvconn_unhash: request for unhash flagged"
		     ", called from %p\n", __builtin_return_address(0));
		return 0;
	}

	spin_lock(&srvconn_lock);

	__tcp_vs_srvconn_unhash(sc);

	spin_unlock(&srvconn_lock);

	return 1;
}


/*
 *	Get a server connection associated with supplied parameters from
 *	the server connection table.
 */
server_conn_t *
tcp_vs_srvconn_get(__u32 addr, __u16 port)
{
	unsigned hash;
	server_conn_t *sc;
	struct list_head *l, *e;

	hash = tcp_vs_srvconn_hashkey(addr, port);
	l = &srvconn_tab[hash];

	/* need disable the bottom half */
	spin_lock(&srvconn_lock);

	for (e = l->next; e != l; e = e->next) {
		sc = list_entry(e, server_conn_t, list);
		if (addr == sc->addr && port == sc->port) {
			/* HIT */
			__tcp_vs_srvconn_unhash(sc);
			tcp_vs_del_slowtimer(&sc->keepalive_timer);
			spin_unlock(&srvconn_lock);
			return sc;
		}
	}

	spin_unlock(&srvconn_lock);

	return NULL;
}


/*
 *      Restart its timer with its timeout and put back the conn.
 */
void
tcp_vs_srvconn_put(server_conn_t * sc)
{
	/* reset it expire in its timeout */
	sc->keepalive_timer.expires = jiffies
	    + sysctl_ktcpvs_keepalive_timeout * HZ;
	tcp_vs_add_slowtimer(&sc->keepalive_timer);

	/* put it back to the table */
	tcp_vs_srvconn_hash(sc);
}

/*
 *	Bind a server connection entry with its server
 *	Called just after a new connection entry is created.
 */
static inline void
tcp_vs_bind_dest(server_conn_t * sc, tcp_vs_dest_t * dest)
{
	/* if dest is NULL, then return directly */
	if (!dest)
		return;

	/* Increase the refcnt counter of the dest */
	atomic_inc(&dest->refcnt);
	sc->dest = dest;

	TCP_VS_DBG(4,
		   "Bind a connection to the server TCP:%u.%u.%u.%u:%d\n",
		   NIPQUAD(sc->addr), ntohs(sc->port));
}


/*
 *  Unbind a connection entry with its VS destination
 *  Called by the tcp_vs_srvconn_expire function.
 */
static inline void
tcp_vs_unbind_dest(server_conn_t * sc)
{
	tcp_vs_dest_t *dest = sc->dest;

	if (!dest)
		return;

	TCP_VS_DBG(4,
		   "Unbind a connection to the server TCP:%u.%u.%u.%u:%d\n",
		   NIPQUAD(sc->addr), ntohs(sc->port));

	sc->dest = NULL;
	atomic_dec(&dest->refcnt);
}


void
tcp_vs_srvconn_free(server_conn_t * sc)
{
	/* close its socket */
	if (sc->sock) {
		sock_release(sc->sock);
	}

	/* unbind it with its dest server */
	tcp_vs_unbind_dest(sc);

	atomic_dec(&srvconn_counter);

	kmem_cache_free(srvconn_cachep, sc);
}


static void
tcp_vs_srvconn_expire(unsigned long data)
{
	server_conn_t *sc = (server_conn_t *) data;

	TCP_VS_DBG(4, "Trying to release the server connection to "
		   "%u.%u.%u.%u:%d\n", NIPQUAD(sc->addr), ntohs(sc->port));

	/*
	 *      unhash it if it is hashed in the srvconn table
	 */
	if (tcp_vs_srvconn_unhash(sc)) {
		tcp_vs_srvconn_free(sc);
		return;
	}

	TCP_VS_DBG(4,
		   "The server connection (%u.%u.%u.%u:%d) expire later",
		   NIPQUAD(sc->addr), ntohs(sc->port));

	tcp_vs_srvconn_put(sc);
}


/*
 *	Create a new connection entry to the specified server.
 */
server_conn_t *
tcp_vs_srvconn_new(tcp_vs_dest_t * dest)
{
	server_conn_t *sc;
	struct socket *sock;

	sc = kmem_cache_alloc(srvconn_cachep, GFP_ATOMIC);
	if (sc == NULL) {
		TCP_VS_ERR_RL
		    ("tcp_vs_srvconn_new: no memory available.\n");
		return NULL;
	}

	/* create a socket to the dest server */
	sock = tcp_vs_connect2dest(dest);
	if (sock == NULL) {
		TCP_VS_ERR_RL("The destination is not available\n");
		kmem_cache_free(srvconn_cachep, sc);
		// for failed connection
		return NULL;
	}

	TCP_VS_DBG(4, "Create a server connection to %u.%u.%u.%u:%d\n",
		   NIPQUAD(dest->addr), ntohs(dest->port));

	memset(sc, 0, sizeof(*sc));
	INIT_LIST_HEAD(&sc->list);
	init_slowtimer(&sc->keepalive_timer);
	sc->keepalive_timer.data = (unsigned long) sc;
	sc->keepalive_timer.function = tcp_vs_srvconn_expire;
	sc->addr = dest->addr;
	sc->port = dest->port;
	sc->sock = sock;

	atomic_inc(&srvconn_counter);

	/* Bind the connection with its destination server */
	tcp_vs_bind_dest(sc, dest);

	return sc;
}


/*
 *      Flush all the connection entries in the tcp_vs_srvconn_tab
 */
static void
tcp_vs_srvconn_flush(void)
{
	int idx;
	server_conn_t *sc;
	struct list_head *l;

      flush_again:
	for (idx = 0; idx < SRVCONN_TAB_SIZE; idx++) {
		/*
		 *  Lock is actually needed in this loop.
		 */
		spin_lock(&srvconn_lock);

		for (l = &srvconn_tab[idx]; !list_empty(l);) {
			sc = list_entry(l->next, server_conn_t, list);

			__tcp_vs_srvconn_unhash(sc);
			tcp_vs_del_slowtimer(&sc->keepalive_timer);
			tcp_vs_srvconn_free(sc);
		}
		spin_unlock(&srvconn_lock);
	}

	/* the counter may be not zero, because maybe some server
	   connection entries are used by the scheduler now. */
	if (atomic_read(&srvconn_counter) != 0) {
		schedule();
		goto flush_again;
	}
}


int
tcp_vs_srvconn_init(void)
{
	int idx;

	/* Allocate the slab cache of server connections */
	srvconn_cachep = kmem_cache_create("tcp_vs_srvconn",
					   sizeof(server_conn_t), 0,
					   SLAB_HWCACHE_ALIGN, NULL, NULL);
	if (!srvconn_cachep) {
		return -ENOMEM;
	}

	for (idx = 0; idx < SRVCONN_TAB_SIZE; idx++) {
		INIT_LIST_HEAD(&srvconn_tab[idx]);
	}

	return 0;
}


void
tcp_vs_srvconn_cleanup(void)
{
	/* flush all the connection entries first */
	tcp_vs_srvconn_flush();

	/* Release the empty cache */
	kmem_cache_destroy(srvconn_cachep);
}
