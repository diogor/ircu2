/*
 * IRC - Internet Relay Chat, ircd/list.c
 * Copyright (C) 1990 Jarkko Oikarinen and
 *                    University of Oulu, Finland
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 1, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * $Id$
 */
#include "list.h"

#include "client.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_reply.h"
#include "ircd_string.h"
#include "listener.h"
#include "match.h"
#include "numeric.h"
#include "res.h"
#include "s_bsd.h"
#include "s_conf.h"
#include "s_debug.h"
#include "s_misc.h"
#include "s_user.h"
#include "send.h"
#include "struct.h"
#include "support.h"
#include "whowas.h"

#include <assert.h>
#include <stddef.h>  /* offsetof */
#include <unistd.h>  /* close */
#include <string.h>

#ifdef DEBUGMODE
static struct liststats {
  int inuse;
} clients, connections, users, servs, links;
#endif

static unsigned int clientAllocCount;
static struct Client* clientFreeList;

static unsigned int connectionAllocCount;
static struct Connection* connectionFreeList;

static unsigned int slinkAllocCount;
static struct SLink* slinkFreeList;

void init_list(void)
{
  struct Client* cptr;
  struct Connection* con;
  int i;
  /*
   * pre-allocate MAXCONNECTIONS clients and connections
   */
  for (i = 0; i < MAXCONNECTIONS; ++i) {
    cptr = (struct Client*) MyMalloc(sizeof(struct Client));
    cli_next(cptr) = clientFreeList;
    clientFreeList = cptr;
    ++clientAllocCount;

    con = (struct Connection*) MyMalloc(sizeof(struct Connection));
    con_next(con) = connectionFreeList;
    connectionFreeList = con;
    ++connectionAllocCount;
  }

#ifdef DEBUGMODE
  memset(&clients, 0, sizeof(clients));
  memset(&connections, 0, sizeof(connections));
  memset(&users, 0, sizeof(users));
  memset(&servs, 0, sizeof(servs));
  memset(&links, 0, sizeof(links));
#endif
}

static struct Client* alloc_client(void)
{
  struct Client* cptr = clientFreeList;

  if (!cptr) {
    cptr = (struct Client*) MyMalloc(sizeof(struct Client));
    ++clientAllocCount;
  } else
    clientFreeList = cli_next(cptr);

#ifdef DEBUGMODE
  clients.inuse++;
#endif

  memset(cptr, 0, sizeof(struct Client));

  return cptr;
}

static void dealloc_client(struct Client* cptr)
{
#ifdef DEBUGMODE
  --clients.inuse;
#endif

  cli_next(cptr) = clientFreeList;
  clientFreeList = cptr;
}

static struct Connection* alloc_connection(void)
{
  struct Connection* con = connectionFreeList;

  if (!con) {
    con = (struct Connection*) MyMalloc(sizeof(struct Connection));
    ++connectionAllocCount;
  } else
    connectionFreeList = con_next(con);

#ifdef DEBUGMODE
  connections.inuse++;
#endif

  memset(con, 0, sizeof(struct Connection));

  return con;
}

static void dealloc_connection(struct Connection* con)
{
  if (con_dns_reply(con))
    --(con_dns_reply(con)->ref_count);
  if (-1 < con_fd(con))
    close(con_fd(con));
  MsgQClear(&(con_sendQ(con)));
  DBufClear(&(con_recvQ(con)));
  if (con_listener(con))
    release_listener(con_listener(con));

#ifdef DEBUGMODE
  --connections.inuse;
#endif

  con_next(con) = connectionFreeList;
  connectionFreeList = con;
}

/*
 * Create a new struct Client structure and set it to initial state.
 *
 *   from == NULL,   create local client (a client connected to a socket).
 *
 *   from != NULL,   create remote client (behind a socket associated with
 *                   the client defined by 'from').
 *                   ('from' is a local client!!).
 */
struct Client* make_client(struct Client *from, int status)
{
  struct Client* cptr = 0;
  struct Connection* con = 0;

  cptr = alloc_client();

  assert(0 != cptr);

  if (!from) { /* local client, allocate a struct Connection */
    con = alloc_connection();

    assert(0 != con);

    con_fd(con) = -1; /* initialize struct Connection */
    con_nextnick(con) = CurrentTime - NICK_DELAY;
    con_nexttarget(con) = CurrentTime - (TARGET_DELAY * (STARTTARGETS - 1));
    con_handler(con) = UNREGISTERED_HANDLER;
    con_client(con) = cptr;

    cli_local(cptr) = 1; /* Set certain fields of the struct Client */
    cli_since(cptr) = cli_lasttime(cptr) = cli_firsttime(cptr) = CurrentTime;
    cli_lastnick(cptr) = TStime();
  } else
    con = cli_connect(from); /* use 'from's connection */

  assert(0 != con);

  cli_connect(cptr) = con; /* set the connection and other fields */
  cli_status(cptr) = status;
  cli_hnext(cptr) = cptr;
  strcpy(cli_username(cptr), "unknown");

  return cptr;
}

void free_client(struct Client* cptr)
{
  if (!cptr)
    return;
  /*
   * forget to remove the client from the hash table?
   */
  assert(cli_hnext(cptr) == cptr);

  if (cli_from(cptr) == cptr) /* in other words, we're local */
    dealloc_connection(cli_connect(cptr)); /* deallocate the connection... */
  dealloc_client(cptr); /* deallocate the client */
}

struct Server *make_server(struct Client *cptr)
{
  struct Server *serv = cli_serv(cptr);

  if (!serv)
  {
    serv = (struct Server*) MyMalloc(sizeof(struct Server));
    assert(0 != serv);
    memset(serv, 0, sizeof(struct Server)); /* All variables are 0 by default */
#ifdef  DEBUGMODE
    servs.inuse++;
#endif
    cli_serv(cptr) = serv;
    cli_serv(cptr)->lag = 60000;
    *serv->by = '\0';
    DupString(serv->last_error_msg, "<>");      /* String must be non-empty */
  }
  return cli_serv(cptr);
}

/*
 * Taken the code from ExitOneClient() for this and placed it here.
 * - avalon
 */
void remove_client_from_list(struct Client *cptr)
{
  if (cli_prev(cptr))
    cli_next(cli_prev(cptr)) = cli_next(cptr);
  else {
    GlobalClientList = cli_next(cptr);
    cli_prev(GlobalClientList) = 0;
  }
  if (cli_next(cptr))
    cli_prev(cli_next(cptr)) = cli_prev(cptr);

  cli_next(cptr) = cli_prev(cptr) = 0;

  if (IsUser(cptr) && cli_user(cptr)) {
    add_history(cptr, 0);
    off_history(cptr);
  }
  if (cli_user(cptr)) {
    free_user(cli_user(cptr));
    cli_user(cptr) = 0;
  }

  if (cli_serv(cptr)) {
    if (cli_serv(cptr)->user) {
      free_user(cli_serv(cptr)->user);
      cli_serv(cptr)->user = 0;
    }
    if (cli_serv(cptr)->client_list)
      MyFree(cli_serv(cptr)->client_list);
    MyFree(cli_serv(cptr)->last_error_msg);
    MyFree(cli_serv(cptr));
#ifdef  DEBUGMODE
    --servs.inuse;
#endif
  }
  free_client(cptr);
}

/*
 * Although only a small routine, it appears in a number of places
 * as a collection of a few lines...functions like this *should* be
 * in this file, shouldnt they ?  after all, this is list.c, isn't it ?
 * -avalon
 */
void add_client_to_list(struct Client *cptr)
{
  /*
   * Since we always insert new clients to the top of the list,
   * this should mean the "me" is the bottom most item in the list.
   * XXX - don't always count on the above, things change
   */
  cli_prev(cptr) = 0;
  cli_next(cptr) = GlobalClientList;
  GlobalClientList = cptr;
  if (cli_next(cptr))
    cli_prev(cli_next(cptr)) = cptr;
}

/*
 * Look for ptr in the linked listed pointed to by link.
 */
struct SLink *find_user_link(struct SLink *lp, struct Client *ptr)
{
  if (ptr) {
    while (lp) {
      if (lp->value.cptr == ptr)
        return (lp);
      lp = lp->next;
    }
  }
  return NULL;
}

struct SLink* make_link(void)
{
  struct SLink* lp = slinkFreeList;
  if (lp)
    slinkFreeList = lp->next;
  else {
    lp = (struct SLink*) MyMalloc(sizeof(struct SLink));
    ++slinkAllocCount;
  }
  assert(0 != lp);
#ifdef  DEBUGMODE
  links.inuse++;
#endif
  return lp;
}

void free_link(struct SLink* lp)
{
  if (lp) {
    lp->next = slinkFreeList;
    slinkFreeList = lp;
  }
#ifdef  DEBUGMODE
  links.inuse--;
#endif
}

struct DLink *add_dlink(struct DLink **lpp, struct Client *cp)
{
  struct DLink* lp = (struct DLink*) MyMalloc(sizeof(struct DLink));
  assert(0 != lp);
  lp->value.cptr = cp;
  lp->prev = 0;
  if ((lp->next = *lpp))
    lp->next->prev = lp;
  *lpp = lp;
  return lp;
}

void remove_dlink(struct DLink **lpp, struct DLink *lp)
{
  assert(0 != lpp);
  assert(0 != lp);

  if (lp->prev) {
    if ((lp->prev->next = lp->next))
      lp->next->prev = lp->prev;
  }
  else if ((*lpp = lp->next))
    lp->next->prev = NULL;
  MyFree(lp);
}

#ifdef  DEBUGMODE
void send_listinfo(struct Client *cptr, char *name)
{
  int inuse = 0, mem = 0, tmp = 0;

  send_reply(cptr, SND_EXPLICIT | RPL_STATSDEBUG, ":Clients: inuse: %d(%d)",
	     clients.inuse, tmp = clients.inuse * sizeof(struct Client));
  mem += tmp;
  inuse += clients.inuse;
  send_reply(cptr, SND_EXPLICIT | RPL_STATSDEBUG, "Connections: inuse: %d(%d)",
	     connections.inuse,
	     tmp = connections.inuse * sizeof(struct Connection));
  mem += tmp;
  inuse += connections.inuse;
  send_reply(cptr, SND_EXPLICIT | RPL_STATSDEBUG, ":Users: inuse: %d(%d)",
	     users.inuse, tmp = users.inuse * sizeof(struct User));
  mem += tmp;
  inuse += users.inuse;
  send_reply(cptr, SND_EXPLICIT | RPL_STATSDEBUG, ":Servs: inuse: %d(%d)",
	     servs.inuse, tmp = servs.inuse * sizeof(struct Server));
  mem += tmp;
  inuse += servs.inuse;
  send_reply(cptr, SND_EXPLICIT | RPL_STATSDEBUG, ":Links: inuse: %d(%d)",
	     links.inuse, tmp = links.inuse * sizeof(struct SLink));
  mem += tmp;
  inuse += links.inuse;
  send_reply(cptr, SND_EXPLICIT | RPL_STATSDEBUG, ":Confs: inuse: %d(%d)",
	     GlobalConfCount, tmp = GlobalConfCount * sizeof(struct ConfItem));
  mem += tmp;
  inuse += GlobalConfCount;
  send_reply(cptr, SND_EXPLICIT | RPL_STATSDEBUG, ":Totals: inuse %d %d",
	     inuse, mem);
}

#endif
