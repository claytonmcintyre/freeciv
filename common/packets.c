/********************************************************************** 
 Freeciv - Copyright (C) 1996 - A Kjeldberg, L Gregersen, P Unold
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
***********************************************************************/
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif

#include "capability.h"
#include "events.h"
#include "log.h"
#include "mem.h"
#include "support.h"

#include "packets.h"

/********************************************************************** 
 The current packet functions don't handle signed values
 correct. This will probably lead to problems when compiling
 freeciv for a platform which has 64 bit ints. Also 16 and 8
 bits values cannot be signed without using tricks (look in e.g
 receive_packet_city_info() )

  TODO to solve these problems:
    o use the new signed functions where they are necessary
    o change the prototypes of the unsigned functions
       to unsigned int instead of int (but only on the 32
       bit functions, because otherwhere it's not necessary)

  Possibe enhancements:
    o check in configure for ints with size others than 4
    o write real put functions and check for the limit
***********************************************************************/


/* A pack_iter is used to iterate through a packet, while ensuring that
 * we don't read off the end of the packet, by comparing (ptr-base) vs len.
 * Note that (ptr-base) gives the number of bytes read so far, and
 * len gives the total number.
 */
struct pack_iter {
  int len;			/* packet length (as claimed by packet) */
  int type;			/* packet type (only here for log output) */
  unsigned char *base;		/* start of packet */
  unsigned char *ptr;		/* address of next data to pull off */
  int swap_bytes;		/* to deal (minimally) with old versions */
  int short_packet;		/* set to 1 if try to read past end */
  int bad_string;		/* set to 1 if received too-long string */
  int bad_bit_string;		/* set to 1 if received bad bit-string */
};

static void pack_iter_init(struct pack_iter *piter, struct connection *pc);
static void pack_iter_end(struct pack_iter *piter, struct connection *pc);
static int  pack_iter_remaining(struct pack_iter *piter);

/* put_uint16 and put_string are non-static for meta.c */

static unsigned char *put_uint8(unsigned char *buffer, int val);
static unsigned char *put_uint32(unsigned char *buffer, int val);

static unsigned char *put_uint8_vec8(unsigned char *buffer, int *val, int stop);
static unsigned char *put_uint16_vec8(unsigned char *buffer, int *val, int stop);

static unsigned char *put_bit_string(unsigned char *buffer, char *str);
static unsigned char *put_city_map(unsigned char *buffer, char *str);
static unsigned char *put_tech_list(unsigned char *buffer, int *techs);

/* iget = iterator versions */
static void iget_uint8(struct pack_iter *piter, int *val);
static void iget_uint16(struct pack_iter *piter, int *val);
static void iget_uint32(struct pack_iter *piter, int *val);

static void iget_uint8_vec8(struct pack_iter *piter, int **val, int stop);
static void iget_uint16_vec8(struct pack_iter *piter, int **val, int stop);

static void iget_string(struct pack_iter *piter, char *mystring, int navail);
static void iget_bit_string(struct pack_iter *piter, char *str, int navail);
static void iget_city_map(struct pack_iter *piter, char *str, int navail);
static void iget_tech_list(struct pack_iter *piter, int *techs);

/* Use the above iget versions instead of the get versions below,
 * except in special cases --dwp */
static unsigned char *get_uint8(unsigned char *buffer, int *val);
static unsigned char *get_uint16(unsigned char *buffer, int *val);
static unsigned char *get_uint32(unsigned char *buffer, int *val);

static unsigned char *get_uint8_vec8(unsigned char *buffer, int **val, int stop);
static unsigned char *get_uint16_vec8(unsigned char *buffer, int **val, int stop);

static unsigned int swab_uint16(unsigned int val);
static unsigned int swab_uint32(unsigned int val);
static void swab_puint16(int *ptr);
static void swab_puint32(int *ptr);


/**************************************************************************
Swap bytes on an integer considered as 16 bits
**************************************************************************/
static unsigned int swab_uint16(unsigned int val)
{
  return ((val & 0xFF) << 8) | ((val & 0xFF00) >> 8);
}

/**************************************************************************
Swap bytes on an integer considered as 32 bits
**************************************************************************/
static unsigned int swab_uint32(unsigned int val)
{
  return ((val & 0xFF) << 24) | ((val & 0xFF00) << 8)
    | ((val & 0xFF0000) >> 8) | ((val & 0xFF000000) >> 24);
}

/**************************************************************************
Swap bytes on an pointed-to integer considered as 16 bits
**************************************************************************/
static void swab_puint16(int *ptr)
{
  (*ptr) = swab_uint16(*ptr);
}

/**************************************************************************
Swap bytes on an pointed-to integer considered as 32 bits
**************************************************************************/
static void swab_puint32(int *ptr)
{
  (*ptr) = swab_uint32(*ptr);
}

/**************************************************************************
...
**************************************************************************/
void *get_packet_from_connection(struct connection *pc, int *ptype)
{
  int len, type;

  if (pc->used==0)
    return NULL;		/* connection was closed, stop reading */
  
  if(pc->buffer->ndata<3)
    return NULL;           /* length and type not read */

  get_uint16(pc->buffer->data, &len);
  get_uint8(pc->buffer->data+2, &type);

  if(pc->first_packet) {
    /* the first packet better be short: */
    freelog(LOG_DEBUG, "first packet type %d len %d", type, len);
    if(len > 1024) {
      freelog(LOG_NORMAL, "connection %s detected as old byte order",
	      conn_description(pc));
      pc->byte_swap = 1;
    } else {
      pc->byte_swap = 0;
    }
    pc->first_packet = 0;
  }

  if(pc->byte_swap) {
    len = swab_uint16(len);
  }

  if(len > pc->buffer->ndata)
    return NULL;           /* not all data has been read */

  /* so the packet gets processed (removed etc) properly: */
  if(pc->byte_swap) {
    put_uint16(pc->buffer->data, len);
  }

  freelog(LOG_DEBUG, "packet type %d len %d", type, len);

  *ptype=type;

  switch(type) {

  case PACKET_REQUEST_JOIN_GAME:
    return receive_packet_req_join_game(pc);

  case PACKET_JOIN_GAME_REPLY:
    return receive_packet_join_game_reply(pc);

  case PACKET_SERVER_SHUTDOWN:
    return receive_packet_generic_message(pc);

  case PACKET_UNIT_INFO:
    return receive_packet_unit_info(pc);

  case PACKET_CITY_INFO:
    return receive_packet_city_info(pc);

  case PACKET_SHORT_CITY:
    return receive_packet_short_city(pc);

  case PACKET_MOVE_UNIT:
    return receive_packet_move_unit(pc);

  case PACKET_TURN_DONE:
    return receive_packet_generic_message(pc);

  case PACKET_BEFORE_NEW_YEAR:
    return receive_packet_before_new_year(pc);
  case PACKET_NEW_YEAR:
    return receive_packet_new_year(pc);

  case PACKET_TILE_INFO:
    return receive_packet_tile_info(pc);

  case PACKET_SELECT_NATION:
    return receive_packet_generic_values(pc);

  case PACKET_REMOVE_UNIT:
  case PACKET_REMOVE_CITY:
  case PACKET_GAME_STATE:
  case PACKET_REPORT_REQUEST:
  case PACKET_REMOVE_PLAYER:  
  case PACKET_CITY_REFRESH:
  case PACKET_INCITE_INQ:
  case PACKET_CITY_NAME_SUGGEST_REQ:
  case PACKET_ADVANCE_FOCUS:
  case PACKET_PLAYER_CANCEL_PACT:
    return receive_packet_generic_integer(pc);
    
  case PACKET_ALLOC_NATION:
    return receive_packet_alloc_nation(pc);

  case PACKET_SHOW_MESSAGE:
    return receive_packet_generic_message(pc);

  case PACKET_PLAYER_INFO:
    return receive_packet_player_info(pc);

  case PACKET_GAME_INFO:
    return receive_packet_game_info(pc);

  case PACKET_MAP_INFO:
    return receive_packet_map_info(pc);

  case PACKET_CHAT_MSG:
  case PACKET_PAGE_MSG:
    return receive_packet_generic_message(pc);
    
  case PACKET_CITY_SELL:
  case PACKET_CITY_BUY:
  case PACKET_CITY_CHANGE:
  case PACKET_CITY_WORKLIST:
  case PACKET_CITY_MAKE_SPECIALIST:
  case PACKET_CITY_MAKE_WORKER:
  case PACKET_CITY_CHANGE_SPECIALIST:
  case PACKET_CITY_RENAME:
    return receive_packet_city_request(pc);

  case PACKET_PLAYER_RATES:
  case PACKET_PLAYER_REVOLUTION:
  case PACKET_PLAYER_GOVERNMENT:
  case PACKET_PLAYER_RESEARCH:
  case PACKET_PLAYER_TECH_GOAL:
  case PACKET_PLAYER_WORKLIST:
    return receive_packet_player_request(pc);

  case PACKET_UNIT_BUILD_CITY:
  case PACKET_UNIT_DISBAND:
  case PACKET_UNIT_CHANGE_HOMECITY:
  case PACKET_UNIT_ESTABLISH_TRADE:
  case PACKET_UNIT_HELP_BUILD_WONDER:
  case PACKET_UNIT_GOTO_TILE:
  case PACKET_UNIT_AUTO:
  case PACKET_UNIT_UNLOAD:
  case PACKET_UNIT_UPGRADE:
  case PACKET_UNIT_NUKE:
  case PACKET_UNIT_PARADROP_TO:
    return receive_packet_unit_request(pc);
  case PACKET_UNIT_CONNECT:
    return receive_packet_unit_connect(pc);
  case PACKET_UNITTYPE_UPGRADE:
    return receive_packet_unittype_info(pc);
  case PACKET_UNIT_COMBAT:
    return receive_packet_unit_combat(pc);
  case PACKET_NUKE_TILE:
    return receive_packet_nuke_tile(pc);
  case PACKET_DIPLOMAT_ACTION:
    return receive_packet_diplomat_action(pc);

  case PACKET_DIPLOMACY_INIT_MEETING:
  case PACKET_DIPLOMACY_CREATE_CLAUSE:
  case PACKET_DIPLOMACY_REMOVE_CLAUSE:
  case PACKET_DIPLOMACY_CANCEL_MEETING:
  case PACKET_DIPLOMACY_ACCEPT_TREATY:
  case PACKET_DIPLOMACY_SIGN_TREATY:
    return receive_packet_diplomacy_info(pc);

  case PACKET_INCITE_COST:
  case PACKET_CITY_OPTIONS:
    return receive_packet_generic_values(pc);

  case PACKET_RULESET_CONTROL:
    return receive_packet_ruleset_control(pc);
  case PACKET_RULESET_TECH:
    return receive_packet_ruleset_tech(pc);
  case PACKET_RULESET_UNIT:
    return receive_packet_ruleset_unit(pc);
  case PACKET_RULESET_BUILDING:
    return receive_packet_ruleset_building(pc);
  case PACKET_RULESET_TERRAIN:
    return receive_packet_ruleset_terrain(pc);
  case PACKET_RULESET_TERRAIN_CONTROL:
    return receive_packet_ruleset_terrain_control(pc);
  case PACKET_RULESET_GOVERNMENT:
    return receive_packet_ruleset_government(pc);
  case PACKET_RULESET_GOVERNMENT_RULER_TITLE:
    return receive_packet_ruleset_government_ruler_title(pc);
  case PACKET_RULESET_NATION:
    return receive_packet_ruleset_nation(pc);
  case PACKET_RULESET_CITY:
    return receive_packet_ruleset_city(pc);
  case PACKET_RULESET_GAME:
    return receive_packet_ruleset_game(pc);

  case PACKET_SPACESHIP_INFO:
    return receive_packet_spaceship_info(pc);

  case PACKET_SPACESHIP_ACTION:
    return receive_packet_spaceship_action(pc);
    
  case PACKET_CITY_NAME_SUGGESTION:
    return receive_packet_city_name_suggestion(pc);

  case PACKET_SABOTAGE_LIST:
    return receive_packet_sabotage_list(pc);

  case PACKET_CONN_INFO:
    return receive_packet_conn_info(pc);

  default:
    freelog(LOG_ERROR, "unknown packet type %d received from %s",
	    type, conn_description(pc));
    remove_packet_from_buffer(pc->buffer);
    return NULL;
  };
}

/**************************************************************************
  Remove the packet from the buffer
**************************************************************************/
void remove_packet_from_buffer(struct socket_packet_buffer *buffer)
{
  int len;
  get_uint16(buffer->data, &len);
  memcpy(buffer->data, buffer->data+len, buffer->ndata-len);
  buffer->ndata-=len;
}

/**************************************************************************
  Initialize pack_iter at the start of receiving a packet.
  Sets all entries in piter.
  There should already be a packet, and it should have at least
  3 bytes, as checked in get_packet_from_connection().
  The length data will already be byte swapped if necessary.
**************************************************************************/
static void pack_iter_init(struct pack_iter *piter, struct connection *pc)
{
  assert(piter!=NULL && pc!=NULL);
  assert(pc->buffer->ndata >= 3);
  
  piter->swap_bytes = pc->byte_swap;
  piter->ptr = piter->base = pc->buffer->data;
  piter->ptr = get_uint16(piter->ptr, &piter->len);
  piter->ptr = get_uint8(piter->ptr, &piter->type);
  piter->short_packet = (piter->len < 3);
  piter->bad_string = 0;
  piter->bad_bit_string = 0;
}

/**************************************************************************
  Returns the number of bytes still unread by pack_iter.
  That is, the length minus the number already read.
  If the packet was already too short previously, returns -1.
**************************************************************************/
static int pack_iter_remaining(struct pack_iter *piter)
{
  assert(piter);
  if (piter->short_packet) {
    return -1;
  } else {
    return piter->len - (piter->ptr - piter->base);
  }
}

/**************************************************************************
  At the end of reading a packet, check if we read the right amount
  of data.  Prints a log message if not.
  Also check bad_string and bad_bit_string flags.
  Note that in the client, *pc->addr is null (static mem, never set),
  and pc->player should be NULL.
**************************************************************************/
static void pack_iter_end(struct pack_iter *piter, struct connection *pc)
{
  int rem;
  char from[MAX_LEN_ADDR+MAX_LEN_NAME+128];
  
  assert(piter!=NULL && pc!=NULL);

  from[0] = '\0';
  rem = pack_iter_remaining(piter);

  /* pack_iter_end is called for every packet, so avoid snprintf
   * unless we know we will need it:
   */
  if (piter->bad_string || piter->bad_bit_string || rem != 0) {
    my_snprintf(from, sizeof(from), " from %s", conn_description(pc));
  }
  
  if (piter->bad_string) {
    freelog(LOG_ERROR,
	    "received bad string in packet (type %d, len %d)%s",
	    piter->type, piter->len, from);
  }
  if (piter->bad_bit_string) {
    freelog(LOG_ERROR,
	    "received bad bit string in packet (type %d, len %d)%s",
	    piter->type, piter->len, from);
  }
  
  if (rem < 0) {
    freelog(LOG_ERROR, "received short packet (type %d, len %d)%s",
	    piter->type, piter->len, from);
  } else if(rem > 0) {
    /* This may be ok, eg a packet from a newer version with extra info
     * which we should just ignore */
    freelog(LOG_VERBOSE, "received long packet (type %d, len %d, rem %d)%s",
	    piter->type, piter->len, rem, from);
  }
}

/**************************************************************************
...
**************************************************************************/
static unsigned char *get_uint8(unsigned char *buffer, int *val)
{
  if(val) {
    *val=(*buffer);
  }
  return buffer+1;
}

/**************************************************************************
...
**************************************************************************/
#ifdef SIGNED_INT_FUNCTIONS
static unsigned char *get_sint8(unsigned char *buffer, int *val)
{
  if(val) {
    int newval = *buffer;

    if(newval > 0x7f) newval -= 0x100;
    *val=newval;
  }
  return buffer+1;
}
#endif

/**************************************************************************
...
**************************************************************************/
static unsigned char *put_uint8(unsigned char *buffer, int val)
{
  *buffer++=val&0xff;
  return buffer;
}

#define put_sint8(b,v) put_uint8(b,v)

/**************************************************************************
...
**************************************************************************/
static unsigned char *get_uint16(unsigned char *buffer, int *val)
{
  if(val) {
    unsigned short x;
    memcpy(&x,buffer,2);
    *val=ntohs(x);
  }
  return buffer+2;
}

/**************************************************************************
...
**************************************************************************/
static unsigned char *get_sint16(unsigned char *buffer, int *val)
{
  if(val) {
    unsigned short x;
    int newval;

    memcpy(&x,buffer,2);
    newval = ntohs(x);
    if(newval > 0x7fff) newval -= 0x10000;
    *val=newval;
  }
  return buffer+2;
}

/**************************************************************************
...
**************************************************************************/
unsigned char *put_uint16(unsigned char *buffer, int val)
{
  unsigned short x = htons(val);
  memcpy(buffer,&x,2);
  return buffer+2;
}

#define put_sint16(b,v) put_uint16(b,v)

/**************************************************************************
...
**************************************************************************/
static unsigned char *get_uint32(unsigned char *buffer, int *val)
{
  if(val) {
    unsigned long x;
    memcpy(&x,buffer,4);
    *val=ntohl(x);
  }
  return buffer+4;
}

/**************************************************************************
...
**************************************************************************/
#ifdef SIGNED_INT_FUNCTIONS
static unsigned char *get_sint32(unsigned char *buffer, int *val)
{
  if(val) {
    unsigned long x;
    int newval;

    memcpy(&x,buffer,4);
    newval = ntohl(x);
    /* only makes sense for systems where sizeof(int) > 4 */
#if INT_MAX == 0x7fffffff
#else
    if(newval>0x7fffffff) newval -= 0x100000000;
#endif
    *val=newval;
  }
  return buffer+4;
}
#endif

/**************************************************************************
...
**************************************************************************/
static unsigned char *put_uint32(unsigned char *buffer, int val)
{
  unsigned long x = htonl(val);
  memcpy(buffer,&x,4);
  return buffer+4;
}

#define put_sint32(b,v) put_uint32(b,v)

/**************************************************************************
  Gets a vector of uint8 values; at most 2^8-1 elements.
  Allocates the return value.
  val can be NULL meaning just read past.
**************************************************************************/
static unsigned char *get_uint8_vec8(unsigned char *buffer, int **val, int stop)
{
  int count, inx;
  buffer = get_uint8(buffer, &count);
  if (val) {
    *val = fc_malloc((count + 1) * sizeof(int));
  }
  for (inx = 0; inx < count; inx++) {
    buffer = get_uint8(buffer, val ? &((*val)[inx]) : NULL);
  }
  if (val) {
    (*val)[inx] = stop;
  }
  return buffer;
}

/**************************************************************************
  Gets a vector of sint8 values; at most 2^8-1 elements.
  Allocates the return value.
  val can be NULL meaning just read past.
**************************************************************************/
#ifdef SIGNED_INT_FUNCTIONS
static unsigned char *get_sint8_vec8(unsigned char *buffer, int **val, int stop)
{
  int count, inx;
  buffer = get_uint8(buffer, &count);
  if (val) {
    *val = fc_malloc((count + 1) * sizeof(int));
  }
  for (inx = 0; inx < count; inx++) {
    buffer = get_sint8(buffer, val ? &((*val)[inx]) : NULL);
  }
  if (val) {
    (*val)[inx] = stop;
  }
  return buffer;
}
#endif

/**************************************************************************
  Puts a vector of sint8 values; at most 2^8-1 elements.
  val can be NULL meaning same as zero-length vector.
**************************************************************************/
static unsigned char *put_uint8_vec8(unsigned char *buffer, int *val, int stop)
{
  unsigned char *pcount = buffer;
  int count;
  buffer = put_uint8(buffer, 0);
  if (val) {
    for (count = 0; *val != stop; count++, val++) {
      buffer = put_uint8(buffer, *val);
    }
    put_uint8(pcount, count);
  }
  return buffer;
}

#define put_sint8_vec8(b,v,s) put_uint8_vec8(b,v,s)

/**************************************************************************
  Gets a vector of uint16 values; at most 2^8-1 elements.
  Allocates the return value.
  val can be NULL meaning just read past.
**************************************************************************/
static unsigned char *get_uint16_vec8(unsigned char *buffer, int **val, int stop)
{
  int count, inx;
  buffer = get_uint8(buffer, &count);
  if (val) {
    *val = fc_malloc((count + 1) * sizeof(int));
  }
  for (inx = 0; inx < count; inx++) {
    buffer = get_uint16(buffer, val ? &((*val)[inx]) : NULL);
  }
  if (val) {
    (*val)[inx] = stop;
  }
  return buffer;
}

/**************************************************************************
  Gets a vector of sint16 values; at most 2^8-1 elements.
  Allocates the return value.
  val can be NULL meaning just read past.
**************************************************************************/
#ifdef SIGNED_INT_FUNCTIONS
static unsigned char *get_sint16_vec8(unsigned char *buffer, int **val, int stop)
{
  int count, inx;
  buffer = get_uint8(buffer, &count);
  if (val) {
    *val = fc_malloc((count + 1) * sizeof(int));
  }
  for (inx = 0; inx < count; inx++) {
    buffer = get_sint16(buffer, val ? &((*val)[inx]) : NULL);
  }
  if (val) {
    (*val)[inx] = stop;
  }
  return buffer;
}
#endif

/**************************************************************************
  Puts a vector of sint16 values; at most 2^8-1 elements.
  val can be NULL meaning same as zero-length vector.
**************************************************************************/
static unsigned char *put_uint16_vec8(unsigned char *buffer, int *val, int stop)
{
  unsigned char *pcount = buffer;
  int count;
  buffer = put_uint8(buffer, 0);
  if (val) {
    for (count = 0; *val != stop; count++, val++) {
      buffer = put_uint16(buffer, *val);
    }
    put_uint8(pcount, count);
  }
  return buffer;
}

#define put_sint16_vec8(b,v,s) put_uint16_vec8(b,v,s)

/**************************************************************************
  Like get_uint8, but using a pack_iter.
  Sets *val to zero for short packets.
  val can be NULL meaning just read past.
**************************************************************************/
static void iget_uint8(struct pack_iter *piter, int *val)
{
  assert(piter);
  if (pack_iter_remaining(piter) < 1) {
    piter->short_packet = 1;
    if (val) *val = 0;
    return;
  }
  piter->ptr = get_uint8(piter->ptr, val);
}

/**************************************************************************
  Like get_int8, but using a pack_iter.
  Sets *val to zero for short packets.
  val can be NULL meaning just read past.
**************************************************************************/
#ifdef SIGNED_INT_FUNCTIONS
static void iget_sint8(struct pack_iter *piter, int *val)
{
  assert(piter);
  if (pack_iter_remaining(piter) < 1) {
    piter->short_packet = 1;
    if (val) *val = 0;
    return;
  }
  piter->ptr = get_sint8(piter->ptr, val);
}
#endif
 
/**************************************************************************
  Like get_uint16, but using a pack_iter.
  Also does byte swapping if required.
  Sets *val to zero for short packets.
  val can be NULL meaning just read past.
**************************************************************************/
static void iget_uint16(struct pack_iter *piter, int *val)
{
  assert(piter);
  if (pack_iter_remaining(piter) < 2) {
    piter->short_packet = 1;
    if (val) *val = 0;
    return;
  }
  piter->ptr = get_uint16(piter->ptr, val);
  if (val && piter->swap_bytes) {
    swab_puint16(val);
  }
}

/**************************************************************************
  Like get_int16, but using a pack_iter.
  Also does byte swapping if required.
  Sets *val to zero for short packets.
  val can be NULL meaning just read past.
**************************************************************************/
static void iget_sint16(struct pack_iter *piter, int *val)
{
  assert(piter);
  if (pack_iter_remaining(piter) < 2) {
    piter->short_packet = 1;
    if (val) *val = 0;
    return;
  }
  piter->ptr = get_sint16(piter->ptr, val);
  if (val && piter->swap_bytes) {
    swab_puint16(val);
  }
}

/**************************************************************************
  Like get_uint32, but using a pack_iter.
  Also does byte swapping if required.
  Sets *val to zero for short packets.
  val can be NULL meaning just read past.
**************************************************************************/
static void iget_uint32(struct pack_iter *piter, int *val)
{
  assert(piter);
  if (pack_iter_remaining(piter) < 4) {
    piter->short_packet = 1;
    if (val) *val = 0;
    return;
  }
  piter->ptr = get_uint32(piter->ptr, val);
  if (val && piter->swap_bytes) {
    swab_puint32(val);
  }
}

/**************************************************************************
  Like get_int32, but using a pack_iter.
  Also does byte swapping if required.
  Sets *val to zero for short packets.
  val can be NULL meaning just read past.
**************************************************************************/
#ifdef SIGNED_INT_FUNCTIONS
static void iget_sint32(struct pack_iter *piter, int *val)
{
  assert(piter);
  if (pack_iter_remaining(piter) < 4) {
    piter->short_packet = 1;
    if (val) *val = 0;
    return;
  }
  piter->ptr = get_sint32(piter->ptr, val);
  if (val && piter->swap_bytes) {
    swab_puint32(val);
  }
}
#endif

/**************************************************************************
  Like get_uint8_vec8, but using a pack_iter.
  Sets *val to NULL for short packets.
  val can be NULL meaning just read past.
**************************************************************************/
static void iget_uint8_vec8(struct pack_iter *piter, int **val, int stop)
{
  int count;
  assert(piter);
  if (pack_iter_remaining(piter) < 1) {
    piter->short_packet = 1;
    if (val) *val = NULL;
    return;
  }
  get_uint8(piter->ptr, &count);	/* don't move pointer past uint8 */
  count += 1;				/* adjust to include count uint8 */
  if (pack_iter_remaining(piter) < count) {
    piter->short_packet = 1;
    if (val) *val = NULL;
    return;
  }
  piter->ptr = get_uint8_vec8(piter->ptr, val, stop);
}

/**************************************************************************
  Like get_sint8_vec8, but using a pack_iter.
  Sets *val to NULL for short packets.
  val can be NULL meaning just read past.
**************************************************************************/
#ifdef SIGNED_INT_FUNCTIONS
static void iget_sint8_vec8(struct pack_iter *piter, int **val, int stop)
{
  int count;
  assert(piter);
  if (pack_iter_remaining(piter) < 1) {
    piter->short_packet = 1;
    if (val) *val = NULL;
    return;
  }
  get_uint8(piter->ptr, &count);	/* don't move pointer past uint8 */
  count += 1;				/* adjust to include count uint8 */
  if (pack_iter_remaining(piter) < count) {
    piter->short_packet = 1;
    if (val) *val = NULL;
    return;
  }
  piter->ptr = get_sint8_vec8(piter->ptr, val, stop);
}
#endif

/**************************************************************************
  Like get_uint16_vec8, but using a pack_iter.
  Sets *val to NULL for short packets.
  val can be NULL meaning just read past.
**************************************************************************/
static void iget_uint16_vec8(struct pack_iter *piter, int **val, int stop)
{
  int count;
  assert(piter);
  if (pack_iter_remaining(piter) < 1) {
    piter->short_packet = 1;
    if (val) *val = NULL;
    return;
  }
  get_uint8(piter->ptr, &count);	/* don't move pointer past uint8 */
  count *= 2;				/* number of bytes in vector */
  count += 1;				/* adjust to include count uint8 */
  if (pack_iter_remaining(piter) < count) {
    piter->short_packet = 1;
    if (val) *val = NULL;
    return;
  }
  piter->ptr = get_uint16_vec8(piter->ptr, val, stop);
}

/**************************************************************************
  Like get_sint16_vec8, but using a pack_iter.
  Sets *val to NULL for short packets.
  val can be NULL meaning just read past.
**************************************************************************/
#ifdef SIGNED_INT_FUNCTIONS
static void iget_sint16_vec8(struct pack_iter *piter, int **val, int stop)
{
  int count;
  assert(piter);
  if (pack_iter_remaining(piter) < 1) {
    piter->short_packet = 1;
    if (val) *val = NULL;
    return;
  }
  get_uint8(piter->ptr, &count);	/* don't move pointer past uint8 */
  count *= 2;				/* number of bytes in vector */
  count += 1;				/* adjust to include count uint8 */
  if (pack_iter_remaining(piter) < count) {
    piter->short_packet = 1;
    if (val) *val = NULL;
    return;
  }
  piter->ptr = get_sint16_vec8(piter->ptr, val, stop);
}
#endif

/**************************************************************************
...
**************************************************************************/
unsigned char *put_string(unsigned char *buffer, char *mystring)
{
  int len = strlen(mystring) + 1;
  memcpy(buffer, mystring, len);
  return buffer+len;
}

/**************************************************************************
  Like old get_string, but using a pack_iter, and navail is the memory
  available in mystring.  If string in packet is too long, a truncated
  string is written into mystring (and set piter->bad_string).
  (The truncated string could be the empty string.)
  Result in mystring is guaranteed to be null-terminated.
  mystring can be NULL, in which case just advance piter, and ignore
  navail.  If mystring is non-NULL, navail must be greater than 0.
**************************************************************************/
static void iget_string(struct pack_iter *piter, char *mystring, int navail)
{
  unsigned char *c;
  int ps_len;			/* length in packet, not including null */
  int len;			/* length to copy, not including null */

  assert(piter);
  assert((navail>0) || (mystring==NULL));

  if (pack_iter_remaining(piter) < 1) {
    piter->short_packet = 1;
    if (mystring) *mystring = '\0';
    return;
  }
  
  /* avoid using strlen (or strcpy) on an (unsigned char*)  --dwp */
  for(c=piter->ptr; *c && (c-piter->base) < piter->len; c++) ;

  if ((c-piter->base) >= piter->len) {
    ps_len = pack_iter_remaining(piter);
    piter->short_packet = 1;
    piter->bad_string = 1;
  } else {
    ps_len = c - piter->ptr;
  }
  len = ps_len;
  if (mystring && navail > 0 && ps_len >= navail) {
    piter->bad_string = 1;
    len = navail-1;
  }
  if (mystring) {
    memcpy(mystring, piter->ptr, len);
    mystring[len] = '\0';
  }
  if (!piter->short_packet) {
    piter->ptr += (ps_len+1);		/* past terminator */
  }
}


/**************************************************************************
  This is to encode the pcity->city_map[]; that is, which city tiles
  are worked/available/unavailable.  str should have these values,
  for the 5x5 map, encoded as '0', '1', and '2' char values.
  But we only send the real info (not corners or centre), and
  pack it into 4 bytes.
**************************************************************************/
static unsigned char *put_city_map(unsigned char *buffer, char *str)
{
  static const int index[20]=
      {1,2,3, 5,6,7,8,9, 10,11, 13,14, 15,16,17,18,19, 21,22,23 };
  int i;

  for(i=0;i<20;i+=5)
    *buffer++ = (str[index[i]]-'0')*81 + (str[index[i+1]]-'0')*27 +
	        (str[index[i+2]]-'0')*9 + (str[index[i+3]]-'0')*3 +
	        (str[index[i+4]]-'0')*1;

  return buffer;
}

/**************************************************************************
  Like old get_city_map, but using a pack_iter, and 'navail' is the
  memory available in str.  That is, reverse the encoding of put_city_map().
  str can be NULL, meaning to just read past city_map.
**************************************************************************/
static void iget_city_map(struct pack_iter *piter, char *str, int navail)
{
  static const int index[21]=
      {1,2,3, 5,6,7,8,9, 10,11, 13,14, 15,16,17,18,19, 21,22,23 };
  int i,j;

  assert(piter);
  assert((navail>0) || (str==NULL));
  
  if (pack_iter_remaining(piter) < 4) {
    piter->short_packet = 1;
  }

  if (str == NULL) {
    if (!piter->short_packet) {
      piter->ptr += 4;
    }
    return;
  }

  assert(navail>=26);

  str[0]='2'; str[4]='2'; str[12]='1'; 
  str[20]='2'; str[24]='2'; str[25]='\0';
  
  for(i=0;i<20;)  {
    if (piter->short_packet) {
      /* put in something sensible? */
      for(j=0; j<5; j++) {
	str[index[i++]]='0';
      }
    } else {
      j=*(piter->ptr)++;
      str[index[i++]]='0'+j/81; j%=81;
      str[index[i++]]='0'+j/27; j%=27;
      str[index[i++]]='0'+j/9; j%=9;
      str[index[i++]]='0'+j/3; j%=3;
      str[index[i++]]='0'+j;
    }
  }
}

/**************************************************************************
...
**************************************************************************/
static unsigned char *put_bit_string(unsigned char *buffer, char *str)
{
  int i,b,n;
  int data;

  n=strlen(str);
  *buffer++=n;
  for(i=0;i<n;)  {
    data=0;
    for(b=0;b<8 && i<n;b++,i++)
      if(str[i]=='1') data|=(1<<b);
    *buffer++=data;
  }

  return buffer;
}

/**************************************************************************
  Like old get_bit_string, but using a pack_iter, and 'navail' is the
  memory available in str.
  The first byte tells us the number of bits in the bit string,
  the rest of the bytes are packed bits.  Writes bytes to str,
  as '0' and '1' characters, null terminated.
  This could be made (slightly?) more efficient (not using iget_uint8(),
  but directly mapipulating piter->ptr), but I couldn't be bothered
  trying to get everything (including short_packet) correct. --dwp
**************************************************************************/
static void iget_bit_string(struct pack_iter *piter, char *str, int navail)
{
  int npack;			/* number claimed in packet */
  int i;			/* iterate the bytes */
  int data;			/* one bytes worth */
  int b;			/* one bits worth */

  assert(piter);
  assert(str!=NULL && navail>0);
  
  if (pack_iter_remaining(piter) < 1) {
    piter->short_packet = 1;
    str[0] = '\0';
    return;
  }

  iget_uint8(piter, &npack);
  if (npack <= 0) {
    piter->bad_bit_string = (npack < 0);
    str[0] = '\0';
    return;
  }

  for(i=0; i<npack ; )  {
    iget_uint8(piter, &data);
    for(b=0; b<8 && i<npack; b++,i++) {
      if (i < navail) {
	if(data&(1<<b)) str[i]='1'; else str[i]='0';
      }
    }
  }
  if (npack < navail) {
    str[npack] = '\0';
  } else {
    str[navail-1] = '\0';
    piter->bad_bit_string = 1;
  }

  if (piter->short_packet) {
    piter->bad_bit_string = 1;
  }
}

/**************************************************************************
  Put list of techs, MAX_NUM_TECH_LIST entries or A_LAST terminated.
  Only puts up to and including terminating entry (or MAX).
**************************************************************************/
static unsigned char *put_tech_list(unsigned char *buffer, int *techs)
{
  int i;
   
  for(i=0; i<MAX_NUM_TECH_LIST; i++) {
    buffer = put_uint8(buffer, techs[i]);
    if (techs[i] == A_LAST)
      break;
  }
  return buffer;
}
    
/**************************************************************************
  Get list of techs inversely to put_tech_list
  (MAX_NUM_TECH_LIST entries or A_LAST terminated).
  Arg for 'techs' should be >=MAX_NUM_TECH_LIST length array.
  Fills trailings entries in 'techs' with A_LAST.
**************************************************************************/
static void iget_tech_list(struct pack_iter *piter, int *techs)
{
  int i;
  
  for(i=0; i<MAX_NUM_TECH_LIST; i++) {
    iget_uint8(piter, &techs[i]);
    if (techs[i] == A_LAST)
      break;
  }
  for(; i<MAX_NUM_TECH_LIST; i++) {
    techs[i] = A_LAST;
  }
}

/**************************************************************************
...
**************************************************************************/
/* when removing "worklist_true_ids" capability,
   may want to remove the 'struct connection *pc' argument (?) */
static unsigned char *put_worklist(unsigned char *buffer, struct worklist *pwl,
				   struct connection *pc)
{
  int i;

  buffer = put_uint8(buffer, pwl->is_valid);
  buffer = put_string(buffer, pwl->name);
  for (i = 0; i < MAX_LEN_WORKLIST; i++) {
/* when removing "worklist_true_ids" capability,
   leave only the code from the *else* clause (send untranslated values) */
if (pc && !has_capability("worklist_true_ids", pc->capability)) {
  if (pwl->wlefs[i] == WEF_END) {
    buffer = put_uint16(buffer, 284);
  } else if (pwl->wlefs[i] == WEF_UNIT) {
    buffer = put_uint16(buffer, pwl->wlids[i] + 68);
  } else {
    buffer = put_uint16(buffer, pwl->wlids[i]);
  }
} else {  /* the following is the code to leave when removing "worklist_true_ids" */
    buffer = put_uint8(buffer, pwl->wlefs[i]);
    buffer = put_uint8(buffer, pwl->wlids[i]);
}
  }

  return buffer;
}

/**************************************************************************
...
**************************************************************************/
/* when removing "worklist_true_ids" capability,
   may want to remove the 'struct connection *pc' argument (?) */
static void iget_worklist(struct pack_iter *piter, struct worklist *pwl,
			  struct connection *pc)
{
  int i;

  iget_uint8(piter, &pwl->is_valid);
  iget_string(piter, pwl->name, MAX_LEN_NAME);
  for (i = 0; i < MAX_LEN_WORKLIST; i++) {
/* when removing "worklist_true_ids" capability,
   leave only the code from the *else* clause (receive untranslated values) */
if (pc && !has_capability("worklist_true_ids", pc->capability)) {
  int val;
  iget_uint16(piter, &val);
  if (val == 284) {
    pwl->wlefs[i] = WEF_END;
    pwl->wlids[i] = 0;
  } else if (val >= 68) {
    pwl->wlefs[i] = WEF_UNIT;
    pwl->wlids[i] = val - 68;
  } else {
    pwl->wlefs[i] = WEF_IMPR;
    pwl->wlids[i] = val;
  }
} else {  /* the following is the code to leave when removing "worklist_true_ids" */
    iget_uint8(piter, (int*)&pwl->wlefs[i]);
    iget_uint8(piter, &pwl->wlids[i]);
}
  }
}
    
/*************************************************************************
...
**************************************************************************/
int send_packet_diplomacy_info(struct connection *pc, enum packet_type pt,
			       struct packet_diplomacy_info *packet)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, pt);
  
  cptr=put_uint32(cptr, packet->plrno0);
  cptr=put_uint32(cptr, packet->plrno1);
  cptr=put_uint32(cptr, packet->plrno_from);
  cptr=put_uint32(cptr, packet->clause_type);
  cptr=put_uint32(cptr, packet->value);
  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}

/*************************************************************************
...
**************************************************************************/
struct packet_diplomacy_info *
receive_packet_diplomacy_info(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_diplomacy_info *preq=
    fc_malloc(sizeof(struct packet_diplomacy_info));

  pack_iter_init(&iter, pc);

  iget_uint32(&iter, &preq->plrno0);
  iget_uint32(&iter, &preq->plrno1);
  iget_uint32(&iter, &preq->plrno_from);
  iget_uint32(&iter, &preq->clause_type);
  iget_uint32(&iter, &preq->value);

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return preq;
}


/*************************************************************************
...
**************************************************************************/
int send_packet_diplomat_action(struct connection *pc, 
				struct packet_diplomat_action *packet)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, PACKET_DIPLOMAT_ACTION);
  
  cptr=put_uint8(cptr, packet->action_type);
  cptr=put_uint16(cptr, packet->diplomat_id);
  cptr=put_uint16(cptr, packet->target_id);
  cptr=put_uint16(cptr, packet->value);
  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}

/*************************************************************************
...
**************************************************************************/
struct packet_diplomat_action *
receive_packet_diplomat_action(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_diplomat_action *preq=
    fc_malloc(sizeof(struct packet_diplomat_action));

  pack_iter_init(&iter, pc);

  iget_uint8(&iter, &preq->action_type);
  iget_uint16(&iter, &preq->diplomat_id);
  iget_uint16(&iter, &preq->target_id);
  iget_uint16(&iter, &preq->value);

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return preq;
}

/*************************************************************************
...
**************************************************************************/
int send_packet_nuke_tile(struct connection *pc, 
			  struct packet_nuke_tile *packet)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, PACKET_NUKE_TILE);
  
  cptr=put_uint8(cptr, packet->x);
  cptr=put_uint8(cptr, packet->y);
  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}


/*************************************************************************
...
**************************************************************************/
struct packet_nuke_tile *
receive_packet_nuke_tile(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_nuke_tile *preq=
    fc_malloc(sizeof(struct packet_nuke_tile));

  pack_iter_init(&iter, pc);

  iget_uint8(&iter, &preq->x);
  iget_uint8(&iter, &preq->y);

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return preq;
}


/*************************************************************************
...
**************************************************************************/
int send_packet_unit_combat(struct connection *pc, 
			    struct packet_unit_combat *packet)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, PACKET_UNIT_COMBAT);
  
  cptr=put_uint16(cptr, packet->attacker_unit_id);
  cptr=put_uint16(cptr, packet->defender_unit_id);
  cptr=put_uint8(cptr, packet->attacker_hp);
  cptr=put_uint8(cptr, packet->defender_hp);
  cptr=put_uint8(cptr, packet->make_winner_veteran);
  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}


/*************************************************************************
...
**************************************************************************/
struct packet_unit_combat *
receive_packet_unit_combat(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_unit_combat *preq=
    fc_malloc(sizeof(struct packet_unit_combat));

  pack_iter_init(&iter, pc);

  iget_uint16(&iter, &preq->attacker_unit_id);
  iget_uint16(&iter, &preq->defender_unit_id);
  iget_uint8(&iter, &preq->attacker_hp);
  iget_uint8(&iter, &preq->defender_hp);
  iget_uint8(&iter, &preq->make_winner_veteran);

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return preq;
}


/*************************************************************************
...
**************************************************************************/
int send_packet_unit_request(struct connection *pc, 
			     struct packet_unit_request *packet,
			     enum packet_type req_type)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, req_type);
  cptr=put_uint16(cptr, packet->unit_id);
  cptr=put_uint16(cptr, packet->city_id);
  cptr=put_uint8(cptr, packet->x);
  cptr=put_uint8(cptr, packet->y);
  cptr=put_string(cptr, packet->name);
  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}


/*************************************************************************
...
**************************************************************************/
struct packet_unit_request *
receive_packet_unit_request(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_unit_request *preq=
    fc_malloc(sizeof(struct packet_unit_request));

  pack_iter_init(&iter, pc);

  iget_uint16(&iter, &preq->unit_id);
  iget_uint16(&iter, &preq->city_id);
  iget_uint8(&iter, &preq->x);
  iget_uint8(&iter, &preq->y);
  iget_string(&iter, preq->name, sizeof(preq->name));

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return preq;
}

/*************************************************************************
...
**************************************************************************/
int send_packet_unit_connect(struct connection *pc, 
			     struct packet_unit_connect *packet)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, PACKET_UNIT_CONNECT);
  
  cptr=put_uint8(cptr, packet->activity_type);
  cptr=put_uint16(cptr, packet->unit_id);
  cptr=put_uint16(cptr, packet->dest_x);
  cptr=put_uint16(cptr, packet->dest_y);
  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}

/*************************************************************************
...
**************************************************************************/
struct packet_unit_connect *
receive_packet_unit_connect(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_unit_connect *preq =
    fc_malloc(sizeof(struct packet_unit_connect));

  pack_iter_init(&iter, pc);

  iget_uint8(&iter, &preq->activity_type);
  iget_uint16(&iter, &preq->unit_id);
  iget_uint16(&iter, &preq->dest_x);
  iget_uint16(&iter, &preq->dest_y);

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return preq;
}

/*************************************************************************
...
**************************************************************************/
int send_packet_player_request(struct connection *pc, 
			       struct packet_player_request *packet,
			       enum packet_type req_type)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;

  /* The following is a hack to avoid changing lots of individual
     pieces of code which actually only use a part of this packet
     and ignore the worklist stuff.  IMO the separate uses should
     use separate packets, to avoid this problem (and would also
     reduce amount sent).  --dwp
  */
  if (req_type != PACKET_PLAYER_WORKLIST) {
    packet->worklist.name[0] = '\0';
  }

  cptr=put_uint8(buffer+2, req_type);
  cptr=put_uint8(cptr, packet->tax);
  cptr=put_uint8(cptr, packet->luxury);
  cptr=put_uint8(cptr, packet->science);
  cptr=put_uint8(cptr, packet->government);
  cptr=put_uint8(cptr, packet->tech);
/* when removing "worklist_true_ids" capability,
   may want to remove the 'pc' argument (?) */
  cptr=put_worklist(cptr, &packet->worklist, pc);
  cptr=put_uint8(cptr, packet->wl_idx);
  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}


/*************************************************************************
...
**************************************************************************/
struct packet_player_request *
receive_packet_player_request(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_player_request *preq=
    fc_malloc(sizeof(struct packet_player_request));

  pack_iter_init(&iter, pc);

  iget_uint8(&iter, &preq->tax);
  iget_uint8(&iter, &preq->luxury);
  iget_uint8(&iter, &preq->science);
  iget_uint8(&iter, &preq->government);
  iget_uint8(&iter, &preq->tech);
/* when removing "worklist_true_ids" capability,
   may want to remove the 'pc' argument (?) */
  iget_worklist(&iter, &preq->worklist, pc);
  iget_uint8(&iter, &preq->wl_idx);
  
  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return preq;
}


/*************************************************************************
...
**************************************************************************/
int send_packet_city_request(struct connection *pc, 
			     struct packet_city_request *packet,
			     enum packet_type req_type)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, req_type);
  cptr=put_uint16(cptr, packet->city_id);
  cptr=put_uint8(cptr, packet->build_id);
  cptr=put_uint8(cptr, packet->is_build_id_unit_id?1:0);
  cptr=put_uint8(cptr, packet->worker_x);
  cptr=put_uint8(cptr, packet->worker_y);
  cptr=put_uint8(cptr, packet->specialist_from);
  cptr=put_uint8(cptr, packet->specialist_to);
/* when removing "worklist_true_ids" capability,
   may want to remove the 'pc' argument (?) */
  cptr=put_worklist(cptr, &packet->worklist, pc);
  cptr=put_string(cptr, packet->name);
  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}


/*************************************************************************
...
**************************************************************************/
struct packet_city_request *
receive_packet_city_request(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_city_request *preq=
    fc_malloc(sizeof(struct packet_city_request));

  pack_iter_init(&iter, pc);

  iget_uint16(&iter, &preq->city_id);
  iget_uint8(&iter, &preq->build_id);
  iget_uint8(&iter, &preq->is_build_id_unit_id);
  iget_uint8(&iter, &preq->worker_x);
  iget_uint8(&iter, &preq->worker_y);
  iget_uint8(&iter, &preq->specialist_from);
  iget_uint8(&iter, &preq->specialist_to);
/* when removing "worklist_true_ids" capability,
   may want to remove the 'pc' argument (?) */
  iget_worklist(&iter, &preq->worklist, pc);
  iget_string(&iter, preq->name, sizeof(preq->name));
  
  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return preq;
}


/*************************************************************************
...
**************************************************************************/
int send_packet_player_info(struct connection *pc, struct packet_player_info *pinfo)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  int i;

  cptr=put_uint8(buffer+2, PACKET_PLAYER_INFO);
  cptr=put_uint8(cptr, pinfo->playerno);
  cptr=put_string(cptr, pinfo->name);

  cptr=put_uint8(cptr, pinfo->is_male);
  cptr=put_uint8(cptr, pinfo->government);
  cptr=put_uint32(cptr, pinfo->embassy);
  cptr=put_uint8(cptr, pinfo->city_style);
  cptr=put_uint8(cptr, pinfo->nation);
  cptr=put_uint8(cptr, pinfo->turn_done?1:0);
  cptr=put_uint16(cptr, pinfo->nturns_idle);
  cptr=put_uint8(cptr, pinfo->is_alive?1:0);

  cptr=put_uint32(cptr, pinfo->reputation);
  for (i = 0; i < MAX_NUM_PLAYERS + MAX_NUM_BARBARIANS; i++) {
    cptr=put_uint32(cptr, pinfo->diplstates[i].type);
    cptr=put_uint32(cptr, pinfo->diplstates[i].turns_left);
    cptr=put_uint32(cptr, pinfo->diplstates[i].has_reason_to_cancel);
  }

  cptr=put_uint32(cptr, pinfo->gold);
  cptr=put_uint8(cptr, pinfo->tax);
  cptr=put_uint8(cptr, pinfo->science);
  cptr=put_uint8(cptr, pinfo->luxury);

  cptr=put_uint32(cptr, pinfo->researched);
  cptr=put_uint32(cptr, pinfo->researchpoints);
  cptr=put_uint8(cptr, pinfo->researching);

  cptr=put_bit_string(cptr, (char*)pinfo->inventions);
  cptr=put_uint16(cptr, pinfo->future_tech);
  
  cptr=put_uint8(cptr, pinfo->is_connected?1:0);

  /* Remove block when "conn_info" removed: now sent in conn_info packet */
 if (pc && !has_capability("conn_info", pc->capability)) {
  cptr=put_string(cptr, pinfo->addr);
 }
  /* Remove to here */
  
  cptr=put_uint8(cptr, pinfo->revolution);
  cptr=put_uint8(cptr, pinfo->tech_goal);
  cptr=put_uint8(cptr, pinfo->ai?1:0);
  cptr=put_uint8(cptr, pinfo->is_barbarian);

  /* Remove block when "conn_info" removed: now sent in conn_info packet */
 if (pc && !has_capability("conn_info", pc->capability)) {
  cptr=put_string(cptr, pinfo->capability);
 }
  /* Remove to here */
 
  for (i = 0; i < MAX_NUM_WORKLISTS; i++) {
/* when removing "worklist_true_ids" capability,
   may want to remove the 'pc' argument (?) */
    cptr = put_worklist(cptr, &pinfo->worklists[i], pc);
  }

  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}


/*************************************************************************
...
**************************************************************************/
struct packet_player_info *
receive_packet_player_info(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_player_info *pinfo=
     fc_malloc(sizeof(struct packet_player_info));
  int i;

  pack_iter_init(&iter, pc);

  iget_uint8(&iter,  &pinfo->playerno);
  iget_string(&iter, pinfo->name, sizeof(pinfo->name));

  iget_uint8(&iter,  &pinfo->is_male);
  iget_uint8(&iter,  &pinfo->government);
  iget_uint32(&iter,  &pinfo->embassy);
  iget_uint8(&iter,  &pinfo->city_style);
  iget_uint8(&iter,  &pinfo->nation);
  iget_uint8(&iter,  &pinfo->turn_done);
  iget_uint16(&iter,  &pinfo->nturns_idle);
  iget_uint8(&iter,  &pinfo->is_alive);
  
  iget_uint32(&iter, &pinfo->reputation);
  for (i = 0; i < MAX_NUM_PLAYERS + MAX_NUM_BARBARIANS; i++) {
    iget_uint32(&iter, (int*)&pinfo->diplstates[i].type);
    iget_uint32(&iter, &pinfo->diplstates[i].turns_left);
    iget_uint32(&iter, &pinfo->diplstates[i].has_reason_to_cancel);
  }

  iget_uint32(&iter, &pinfo->gold);
  iget_uint8(&iter, &pinfo->tax);
  iget_uint8(&iter, &pinfo->science);
  iget_uint8(&iter, &pinfo->luxury);

  iget_uint32(&iter, &pinfo->researched); /* signed */
  iget_uint32(&iter, &pinfo->researchpoints);
  iget_uint8(&iter, &pinfo->researching);
  iget_bit_string(&iter, (char*)pinfo->inventions, sizeof(pinfo->inventions));
  iget_uint16(&iter, &pinfo->future_tech);

  iget_uint8(&iter, &pinfo->is_connected);
  
  /* Remove block when "conn_info" removed: now sent in conn_info packet */
 if (pc && !has_capability("conn_info", pc->capability)) {
  iget_string(&iter, pinfo->addr, sizeof(pinfo->addr));
 }
  /* Remove to here */
  
  iget_uint8(&iter, &pinfo->revolution);
  iget_uint8(&iter, &pinfo->tech_goal);
  iget_uint8(&iter, &pinfo->ai);
  iget_uint8(&iter, &pinfo->is_barbarian);

  /* Remove block when "conn_info" removed: now sent in conn_info packet */
 if (pc && !has_capability("conn_info", pc->capability)) {
  iget_string(&iter, pinfo->capability, sizeof(pinfo->capability));
 }
  /* Remove to here */
 
  for (i = 0; i < MAX_NUM_WORKLISTS; i++) {
/* when removing "worklist_true_ids" capability,
   may want to remove the 'pc' argument (?) */
    iget_worklist(&iter, &pinfo->worklists[i], pc);
  }

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return pinfo;
}

/*************************************************************************
  Send connection.id as uint32 even though currently only use ushort
  range, in case want to use it for more later, eg global user-id...?
...
**************************************************************************/
int send_packet_conn_info(struct connection *pc,
			  struct packet_conn_info *pinfo)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  int data;

  /* Remove block when "conn_info" removed: */
  if (!(pc && has_capability("conn_info", pc->capability))) {
    return 0;
  }
  /* Remove to here */
 
  cptr=put_uint8(buffer+2, PACKET_CONN_INFO);
  
  cptr=put_uint32(cptr, pinfo->id);
  
  data = pinfo->used ? 1 : 0;
  data |= pinfo->established ? 2 : 0;
  data |= pinfo->observer ? 4 : 0;
  cptr=put_uint8(cptr, data);
  
  cptr=put_uint8(cptr, pinfo->player_num);
  cptr=put_uint8(cptr, pinfo->access_level);
  
  cptr=put_string(cptr, pinfo->name);
  cptr=put_string(cptr, pinfo->addr);
  cptr=put_string(cptr, pinfo->capability);

  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}

/*************************************************************************
...
**************************************************************************/
struct packet_conn_info *
receive_packet_conn_info(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_conn_info *pinfo=
     fc_malloc(sizeof(struct packet_conn_info));
  int data;

  pack_iter_init(&iter, pc);

  iget_uint32(&iter, &pinfo->id);
  
  iget_uint8(&iter, &data);
  pinfo->used = data&1;
  pinfo->established = (data>>=1)&1;
  pinfo->observer = (data>>=1)&1;

  iget_uint8(&iter, &pinfo->player_num);
  iget_uint8(&iter, &data);
  pinfo->access_level = data;

  iget_string(&iter, pinfo->name, sizeof(pinfo->name));
  iget_string(&iter, pinfo->addr, sizeof(pinfo->addr));
  iget_string(&iter, pinfo->capability, sizeof(pinfo->capability));

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return pinfo;
}


/*************************************************************************
...
**************************************************************************/
int send_packet_game_info(struct connection *pc, 
			  struct packet_game_info *pinfo)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  int i;
  
  cptr=put_uint8(buffer+2, PACKET_GAME_INFO);
  cptr=put_uint16(cptr, pinfo->gold);
  cptr=put_uint32(cptr, pinfo->tech);
  cptr=put_uint8(cptr, pinfo->techlevel);

  cptr=put_uint32(cptr, pinfo->skill_level);
  cptr=put_uint16(cptr, pinfo->timeout);
  cptr=put_uint32(cptr, pinfo->end_year);
  cptr=put_uint32(cptr, pinfo->year);
  cptr=put_uint8(cptr, pinfo->min_players);
  cptr=put_uint8(cptr, pinfo->max_players);
  cptr=put_uint8(cptr, pinfo->nplayers);
  cptr=put_uint8(cptr, pinfo->player_idx);
/* when removing "nuclear_fallout" capability,
   leave only the code from the *else* clause (send unmodified values) */
if (pc && !has_capability("nuclear_fallout", pc->capability)) {
  cptr=put_uint32(cptr, pinfo->globalwarming + pinfo->nuclearwinter);
  cptr=put_uint32(cptr, pinfo->heating + pinfo->cooling);
} else {  /* the following is the code to leave when removing "nuclear_fallout" */
  cptr=put_uint32(cptr, pinfo->globalwarming);
  cptr=put_uint32(cptr, pinfo->heating);
}
if (pc && has_capability("nuclear_fallout", pc->capability)) {
  cptr=put_uint32(cptr, pinfo->nuclearwinter);
  cptr=put_uint32(cptr, pinfo->cooling);
}
  cptr=put_uint8(cptr, pinfo->cityfactor);
  cptr=put_uint8(cptr, pinfo->diplcost);
  cptr=put_uint8(cptr, pinfo->freecost);
  cptr=put_uint8(cptr, pinfo->conquercost);
  cptr=put_uint8(cptr, pinfo->unhappysize);
  
  for(i=0; i<A_LAST/*game.num_tech_types*/; i++)
    cptr=put_uint8(cptr, pinfo->global_advances[i]);
/* when removing "indef_impr_types" capability,
   leave only the code from the *else* clause (send B_LAST values) */
if (pc && !has_capability("indef_impr_types", pc->capability)) {
  for(i=0; i<B_LAST_ENUM; i++)
    cptr=put_uint16(cptr, pinfo->global_wonders[i]);
} else {  /* the following is the code to leave when removing "indef_impr_types" */
  for(i=0; i<B_LAST/*game.num_impr_types*/; i++)
    cptr=put_uint16(cptr, pinfo->global_wonders[i]);
}
  cptr=put_uint8(cptr, pinfo->techpenalty);
  cptr=put_uint8(cptr, pinfo->foodbox);
  cptr=put_uint8(cptr, pinfo->civstyle);
  cptr=put_uint8(cptr, pinfo->spacerace);

  /* computed values */
  cptr=put_uint32(cptr, pinfo->seconds_to_turndone);

  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}

/*************************************************************************
...
**************************************************************************/
struct packet_game_info *receive_packet_game_info(struct connection *pc)
{
  int i;
  struct pack_iter iter;
  struct packet_game_info *pinfo=
     fc_malloc(sizeof(struct packet_game_info));

  pack_iter_init(&iter, pc);

  iget_uint16(&iter, &pinfo->gold);
  iget_uint32(&iter, &pinfo->tech);
  iget_uint8(&iter, &pinfo->techlevel);
  iget_uint32(&iter, &pinfo->skill_level);
  iget_uint16(&iter, &pinfo->timeout);
  iget_uint32(&iter, &pinfo->end_year);
  iget_uint32(&iter, &pinfo->year);
  iget_uint8(&iter, &pinfo->min_players);
  iget_uint8(&iter, &pinfo->max_players);
  iget_uint8(&iter, &pinfo->nplayers);
  iget_uint8(&iter, &pinfo->player_idx);
  iget_uint32(&iter, &pinfo->globalwarming);
  iget_uint32(&iter, &pinfo->heating);
if (pc && has_capability("nuclear_fallout", pc->capability)) {
  iget_uint32(&iter, &pinfo->nuclearwinter);
  iget_uint32(&iter, &pinfo->cooling);
} else {
  pinfo->nuclearwinter = 0;
  pinfo->cooling = 0;
}
  iget_uint8(&iter, &pinfo->cityfactor);
  iget_uint8(&iter, &pinfo->diplcost);
  iget_uint8(&iter, &pinfo->freecost);
  iget_uint8(&iter, &pinfo->conquercost);
  iget_uint8(&iter, &pinfo->unhappysize);
  
  for(i=0; i<A_LAST/*game.num_tech_types*/; i++)
    iget_uint8(&iter, &pinfo->global_advances[i]);
/* when removing "indef_impr_types" capability,
   leave only the code from the *else* clause (receive B_LAST values) */
if (pc && !has_capability("indef_impr_types", pc->capability)) {
  for(i=0; i<B_LAST_ENUM; i++)
    iget_uint16(&iter, &pinfo->global_wonders[i]);
  for( ; i<B_LAST/*game.num_impr_types*/; i++)
    pinfo->global_wonders[i] = 0;
} else {  /* the following is the code to leave when removing "indef_impr_types" */
  for(i=0; i<B_LAST/*game.num_impr_types*/; i++)
    iget_uint16(&iter, &pinfo->global_wonders[i]);
}
  iget_uint8(&iter, &pinfo->techpenalty);
  iget_uint8(&iter, &pinfo->foodbox);
  iget_uint8(&iter, &pinfo->civstyle);
  iget_uint8(&iter, &pinfo->spacerace);

  /* computed values */
  iget_uint32(&iter, &pinfo->seconds_to_turndone);

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return pinfo;
}

/*************************************************************************
...
**************************************************************************/
int send_packet_map_info(struct connection *pc, 
			 struct packet_map_info *pinfo)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;

  cptr=put_uint8(buffer+2, PACKET_MAP_INFO);
  cptr=put_uint8(cptr, pinfo->xsize);
  cptr=put_uint8(cptr, pinfo->ysize);
  cptr=put_uint8(cptr, pinfo->is_earth?1:0);
  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}

/*************************************************************************
...
**************************************************************************/
struct packet_map_info *receive_packet_map_info(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_map_info *pinfo=
     fc_malloc(sizeof(struct packet_map_info));

  pack_iter_init(&iter, pc);

  iget_uint8(&iter, &pinfo->xsize);
  iget_uint8(&iter, &pinfo->ysize);
  iget_uint8(&iter, &pinfo->is_earth);

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return pinfo;
}

/*************************************************************************
...
**************************************************************************/
struct packet_tile_info *
receive_packet_tile_info(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_tile_info *packet=
    fc_malloc(sizeof(struct packet_tile_info));

  pack_iter_init(&iter, pc);

  iget_uint8(&iter, &packet->x);
  iget_uint8(&iter, &packet->y);
  iget_uint8(&iter, &packet->type);
  iget_uint16(&iter, &packet->special);
  iget_uint8(&iter, &packet->known);

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

struct packet_unittype_info *
receive_packet_unittype_info(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_unittype_info *packet=
    fc_malloc(sizeof(struct packet_unittype_info));

  pack_iter_init(&iter, pc);

  iget_uint8(&iter, &packet->type);
  iget_uint8(&iter, &packet->action);
  
  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/*************************************************************************
...
**************************************************************************/
int send_packet_tile_info(struct connection *pc, 
			  struct packet_tile_info *pinfo)

{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;

  cptr=put_uint8(buffer+2, PACKET_TILE_INFO);
  cptr=put_uint8(cptr, pinfo->x);
  cptr=put_uint8(cptr, pinfo->y);
  cptr=put_uint8(cptr, pinfo->type);
/* when removing "nuclear_fallout" capability,
   leave only the code from the *else* clause (send unmodified special) */
if (pc && !has_capability("nuclear_fallout", pc->capability)) {
if (pinfo->special & S_FALLOUT)
cptr=put_uint16(cptr, pinfo->special | S_POLLUTION);
else
cptr=put_uint16(cptr, pinfo->special);
} else {  /* the following is the code to leave when removing "nuclear_fallout" */
  cptr=put_uint16(cptr, pinfo->special);
}
  cptr=put_uint8(cptr, pinfo->known);
  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}

/**************************************************************************
...
**************************************************************************/
int send_packet_new_year(struct connection *pc, struct 
			 packet_new_year *request)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, PACKET_NEW_YEAR);
  cptr=put_uint32(cptr, request->year);
  put_uint16(buffer, cptr-buffer);
  return send_connection_data(pc, buffer, cptr-buffer);
}

/**************************************************************************
...
**************************************************************************/
int send_packet_unittype_info(struct connection *pc, int type, int action)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, PACKET_UNITTYPE_UPGRADE);
  cptr=put_uint8(cptr, type);
  cptr=put_uint8(cptr, action);
  put_uint16(buffer, cptr-buffer);
  return send_connection_data(pc, buffer, cptr-buffer);
}

/**************************************************************************
...
**************************************************************************/
struct packet_before_new_year *
receive_packet_before_new_year(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_before_new_year *packet=
    fc_malloc(sizeof(struct packet_before_new_year));

  pack_iter_init(&iter, pc);
  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/**************************************************************************
...
(Used to be called send_packet_before_end_year; changed for consistency
-- referred to as "before new year" in more places.)
**************************************************************************/
int send_packet_before_new_year(struct connection *pc)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, PACKET_BEFORE_NEW_YEAR);
  put_uint16(buffer, cptr-buffer);
  return send_connection_data(pc, buffer, cptr-buffer);
}

/*************************************************************************
...
**************************************************************************/
int send_packet_unit_info(struct connection *pc, 
			    struct packet_unit_info *req)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  unsigned char pack;

/* when removing "diplomat_investigate_fix" capability,
remove this *entire* piece of code (to REMOVE TO HERE, below) */
if (pc && !has_capability("diplomat_investigate_fix", pc->capability)) {
if (req->packet_use != UNIT_INFO_IDENTITY) {
return 0;
}
}
/* REMOVE TO HERE */

  cptr=put_uint8(buffer+2, PACKET_UNIT_INFO);
  cptr=put_uint16(cptr, req->id);
  cptr=put_uint8(cptr, req->owner);
  pack=(req->select_it   ? 0x04 : 0)
    |  (req->carried     ? 0x08 : 0)
    |  (req->veteran     ? 0x10 : 0)
    |  (req->ai          ? 0x20 : 0)
    |  (req->paradropped ? 0x40 : 0)
    |  (req->connecting  ? 0x80 : 0);
  cptr=put_uint8(cptr, pack);
  cptr=put_uint8(cptr, req->x);
  cptr=put_uint8(cptr, req->y);
  cptr=put_uint16(cptr, req->homecity);
  cptr=put_uint8(cptr, req->type);
  cptr=put_uint8(cptr, req->movesleft);
  cptr=put_uint8(cptr, req->hp);
  cptr=put_uint8(cptr, req->upkeep);
  cptr=put_uint8(cptr, req->upkeep_food);
  cptr=put_uint8(cptr, req->upkeep_gold);
  cptr=put_uint8(cptr, req->unhappiness);
/* when removing "nuclear_fallout" capability,
   leave only the code from the *else* clause (send unmodified activity) */
if (pc && !has_capability("nuclear_fallout", pc->capability)) {
if (req->activity == ACTIVITY_FALLOUT)
cptr=put_uint8(cptr, ACTIVITY_POLLUTION);
else
cptr=put_uint8(cptr, req->activity);
} else {  /* the following is the code to leave when removing "nuclear_fallout" */
  cptr=put_uint8(cptr, req->activity);
}
  cptr=put_uint8(cptr, req->activity_count);
  cptr=put_uint8(cptr, req->goto_dest_x);
  cptr=put_uint8(cptr, req->goto_dest_y);
  cptr=put_uint16(cptr, req->activity_target);
if (pc && has_capability("diplomat_investigate_fix", pc->capability)) {
  cptr=put_uint8(cptr, req->packet_use);
  cptr=put_uint16(cptr, req->info_city_id);
  cptr=put_uint16(cptr, req->serial_num);
}
  if(req->fuel) cptr=put_uint8(cptr, req->fuel);

  put_uint16(buffer, cptr-buffer);
  return send_connection_data(pc, buffer, cptr-buffer);
}

/*************************************************************************
...
**************************************************************************/
int send_packet_city_info(struct connection *pc, struct packet_city_info *req)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  int data;
  cptr=put_uint8(buffer+2, PACKET_CITY_INFO);
  cptr=put_uint16(cptr, req->id);
  cptr=put_uint8(cptr, req->owner);
  cptr=put_uint8(cptr, req->x);
  cptr=put_uint8(cptr, req->y);
  cptr=put_string(cptr, req->name);
  
  cptr=put_uint8(cptr, req->size);
  cptr=put_uint8(cptr, req->ppl_happy);
  cptr=put_uint8(cptr, req->ppl_content);
  cptr=put_uint8(cptr, req->ppl_unhappy);
  cptr=put_uint8(cptr, req->ppl_elvis);
  cptr=put_uint8(cptr, req->ppl_scientist);
  cptr=put_uint8(cptr, req->ppl_taxman);

  cptr=put_uint8(cptr, req->food_prod);
  cptr=put_uint8(cptr, req->food_surplus);
  cptr=put_uint16(cptr, req->shield_prod);
  cptr=put_uint16(cptr, req->shield_surplus);
  cptr=put_uint16(cptr, req->trade_prod);
  cptr=put_uint16(cptr, req->corruption);

  cptr=put_uint16(cptr, req->luxury_total);
  cptr=put_uint16(cptr, req->tax_total);
  cptr=put_uint16(cptr, req->science_total);

  cptr=put_uint16(cptr, req->food_stock);
  cptr=put_uint16(cptr, req->shield_stock);
  cptr=put_uint16(cptr, req->pollution);
  cptr=put_uint8(cptr, req->currently_building);

if (pc && has_capability("production_change_fix", pc->capability)) {
  cptr=put_sint16(cptr, req->turn_last_built);
  cptr=put_sint16(cptr, req->turn_changed_target);
  cptr=put_uint8(cptr, req->changed_from_id);
  cptr=put_uint16(cptr, req->before_change_shields);
}

/* when removing "worklist_true_ids" capability,
   may want to remove the 'pc' argument (?) */
  cptr=put_worklist(cptr, &req->worklist, pc);

  data=req->is_building_unit?1:0;
  data|=req->did_buy?2:0;
  data|=req->did_sell?4:0;
  data|=req->was_happy?8:0;
  data|=req->airlift?16:0;
  data|=req->diplomat_investigate?32:0; /* gentler implementation -- Syela */
if (pc && has_capability("production_change_fix", pc->capability)) {
  data|=req->changed_from_is_unit?64:0;
}
  cptr=put_uint8(cptr, data);

  cptr=put_city_map(cptr, (char*)req->city_map);
  cptr=put_bit_string(cptr, (char*)req->improvements);

  /* only 8 options allowed before need to extend protocol */
  cptr=put_uint8(cptr, req->city_options);
  
  for(data=0;data<4;data++)  {
    if(req->trade[data])  {
      cptr=put_uint16(cptr, req->trade[data]);
      cptr=put_uint8(cptr,req->trade_value[data]);
    }
  }

  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}

/*************************************************************************
...
**************************************************************************/
struct packet_city_info *
receive_packet_city_info(struct connection *pc)
{
  int data;
  struct pack_iter iter;
  struct packet_city_info *packet=
    fc_malloc(sizeof(struct packet_city_info));

  pack_iter_init(&iter, pc);

  iget_uint16(&iter, &packet->id);
  iget_uint8(&iter, &packet->owner);
  iget_uint8(&iter, &packet->x);
  iget_uint8(&iter, &packet->y);
  iget_string(&iter, packet->name, sizeof(packet->name));
  
  iget_uint8(&iter, &packet->size);
  iget_uint8(&iter, &packet->ppl_happy);
  iget_uint8(&iter, &packet->ppl_content);
  iget_uint8(&iter, &packet->ppl_unhappy);
  iget_uint8(&iter, &packet->ppl_elvis);
  iget_uint8(&iter, &packet->ppl_scientist);
  iget_uint8(&iter, &packet->ppl_taxman);
  
  iget_uint8(&iter, &packet->food_prod);
  iget_uint8(&iter, &packet->food_surplus);
  if(packet->food_surplus > 127) packet->food_surplus-=256;
  iget_uint16(&iter, &packet->shield_prod);
  iget_uint16(&iter, &packet->shield_surplus);
  if(packet->shield_surplus > 32767) packet->shield_surplus-=65536;
  iget_uint16(&iter, &packet->trade_prod);
  iget_uint16(&iter, &packet->corruption);

  iget_uint16(&iter, &packet->luxury_total);
  iget_uint16(&iter, &packet->tax_total);
  iget_uint16(&iter, &packet->science_total);
  
  iget_uint16(&iter, &packet->food_stock);
  iget_uint16(&iter, &packet->shield_stock);
  iget_uint16(&iter, &packet->pollution);
  iget_uint8(&iter, &packet->currently_building);

if (pc && has_capability("production_change_fix", pc->capability)) {
  iget_sint16(&iter, &packet->turn_last_built);
  iget_sint16(&iter, &packet->turn_changed_target);
  iget_uint8(&iter, &packet->changed_from_id);
  iget_uint16(&iter, &packet->before_change_shields);
} else {
  packet->turn_last_built = GAME_START_YEAR;
  packet->turn_changed_target = GAME_START_YEAR;
  packet->changed_from_id = packet->currently_building;
  packet->before_change_shields = packet->shield_stock;
}

/* when removing "worklist_true_ids" capability,
   may want to remove the 'pc' argument (?) */
  iget_worklist(&iter, &packet->worklist, pc);

  iget_uint8(&iter, &data);
  packet->is_building_unit = data&1;
  packet->did_buy = (data>>=1)&1;
  packet->did_sell = (data>>=1)&1;
  packet->was_happy = (data>>=1)&1;
  packet->airlift = (data>>=1)&1;
  packet->diplomat_investigate = (data>>=1)&1;
if (pc && has_capability("production_change_fix", pc->capability)) {
  packet->changed_from_is_unit = (data>>=1)&1;
} else {
  packet->changed_from_is_unit = packet->is_building_unit;
}

  iget_city_map(&iter, (char*)packet->city_map, sizeof(packet->city_map));
  iget_bit_string(&iter, (char*)packet->improvements,
		  sizeof(packet->improvements));

  iget_uint8(&iter, &packet->city_options);

  for(data=0;data<4;data++)  {
    if (pack_iter_remaining(&iter) < 3)
      break;
    iget_uint16(&iter, &packet->trade[data]);
    iget_uint8(&iter, &packet->trade_value[data]);
  }
  for(;data<4;data++) packet->trade_value[data]=packet->trade[data]=0;

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/*************************************************************************
...
**************************************************************************/
int send_packet_short_city(struct connection *pc, struct packet_short_city *req)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  int i;

  if (pc && !has_capability("packet_short_city", pc->capability)) {

    /* Send packet_city_info instead. */
    struct packet_city_info old;
    char *p;
    int x, y;

    old.id                 = req->id;
    old.owner              = req->owner;
    old.x                  = req->x;
    old.y                  = req->y;
    sz_strlcpy(old.name, req->name);
    old.size               = req->size;
    old.ppl_happy          = 0;
    if (req->happy) {
      old.ppl_content        = req->size;
      old.ppl_unhappy        = 0;
    } else {
      old.ppl_content        = 0;
      old.ppl_unhappy        = req->size;
    }
    old.ppl_elvis          = req->size;
    old.ppl_scientist      = 0;
    old.ppl_taxman         = 0;
    for (i=0;i<4;i++)  {
      old.trade[i]=0;
      old.trade_value[i]     = 0;
    }
    old.food_prod          = 0;
    old.food_surplus       = 0;
    old.shield_prod        = 0;
    old.shield_surplus     = 0;
    old.trade_prod         = 0;
    old.corruption         = 0;
    old.luxury_total       = 0;
    old.tax_total          = 0;
    old.science_total      = 0;
    old.food_stock         = 0;
    old.shield_stock       = 0;
    old.pollution          = 0;
    old.city_options       = 0;
    old.is_building_unit   = 0;
    old.currently_building = 0;
    init_worklist(&old.worklist);
    old.diplomat_investigate = 0;
    old.airlift            = 0;
    old.did_buy            = 0;
    old.did_sell           = 0;
    old.was_happy          = 0;

    p=old.improvements;
    for(i=0; i<game.num_impr_types; i++)
      *p++ = '0';
    if (req->capital)
      old.improvements[B_PALACE] = '1';
    if (req->walls)
      old.improvements[B_CITY]   = '1';
    *p='\0';

    p=old.city_map;
    for(y=0; y<CITY_MAP_SIZE; y++) /* (Mis)use of function parameters */
      for(x=0; x<CITY_MAP_SIZE; x++)
        *p++=C_TILE_EMPTY+'0';
    *p='\0';

    return send_packet_city_info(pc, &old);
  } /* !has_capability(packet_short_city) */


  cptr=put_uint8(buffer+2, PACKET_SHORT_CITY);
  cptr=put_uint16(cptr, req->id);
  cptr=put_uint8(cptr, req->owner);
  cptr=put_uint8(cptr, req->x);
  cptr=put_uint8(cptr, req->y);
  cptr=put_string(cptr, req->name);
  
  cptr=put_uint8(cptr, req->size);

  i = (req->happy?1:0) | (req->capital?2:0) | (req->walls?4:0);
  cptr=put_uint8(cptr, i);

  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}


/*************************************************************************
...
**************************************************************************/
struct packet_short_city *
receive_packet_short_city(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_short_city *packet=
    fc_malloc(sizeof(struct packet_short_city));
  int i;

  pack_iter_init(&iter, pc);

  iget_uint16(&iter, &packet->id);
  iget_uint8(&iter, &packet->owner);
  iget_uint8(&iter, &packet->x);
  iget_uint8(&iter, &packet->y);
  iget_string(&iter, packet->name, sizeof(packet->name));
  
  iget_uint8(&iter, &packet->size);

  iget_uint8(&iter, &i);
  packet->happy   = i & 1;
  packet->capital = i & 2;
  packet->walls   = i & 4;

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/*************************************************************************
...
**************************************************************************/
struct packet_unit_info *
receive_packet_unit_info(struct connection *pc)
{
  int pack;
  struct pack_iter iter;
  struct packet_unit_info *packet=
    fc_malloc(sizeof(struct packet_unit_info));

  pack_iter_init(&iter, pc);

  iget_uint16(&iter, &packet->id);
  iget_uint8(&iter, &packet->owner);
  iget_uint8(&iter, &pack);
  packet->veteran     = (pack&0x10) ? 1 : 0;
  packet->ai          = (pack&0x20) ? 1 : 0;
  packet->paradropped = (pack&0x40) ? 1 : 0;
  packet->connecting  = (pack&0x80) ? 1 : 0;
  packet->carried     = (pack&0x08) ? 1 : 0;
  packet->select_it   = (pack&0x04) ? 1 : 0;
  iget_uint8(&iter, &packet->x);
  iget_uint8(&iter, &packet->y);
  iget_uint16(&iter, &packet->homecity);
  iget_uint8(&iter, &packet->type);
  iget_uint8(&iter, &packet->movesleft);
  iget_uint8(&iter, &packet->hp);
  iget_uint8(&iter, &packet->upkeep);
  iget_uint8(&iter, &packet->upkeep_food);
  iget_uint8(&iter, &packet->upkeep_gold);
  iget_uint8(&iter, &packet->unhappiness);
  iget_uint8(&iter, &packet->activity);
/* when removing "nuclear_fallout" capability,
   remove this *entire* piece of code (to REMOVE TO HERE, below) */
if (pc && !has_capability("nuclear_fallout", pc->capability)) {
if ((packet->activity == ACTIVITY_POLLUTION) &&
    (map_get_special(packet->x, packet->y) & S_FALLOUT)) {
packet->activity = ACTIVITY_FALLOUT;
}
}
/* REMOVE TO HERE */
  iget_uint8(&iter, &packet->activity_count);
  iget_uint8(&iter, &packet->goto_dest_x);
  iget_uint8(&iter, &packet->goto_dest_y);
  iget_uint16(&iter, &packet->activity_target);
if (pc && has_capability("diplomat_investigate_fix", pc->capability)) {
  iget_uint8(&iter, &packet->packet_use);
  iget_uint16(&iter, &packet->info_city_id);
  iget_uint16(&iter, &packet->serial_num);
} else {
  packet->packet_use=UNIT_INFO_IDENTITY;
  packet->info_city_id=0;
  packet->serial_num=0;
}
  if (pack_iter_remaining(&iter) >= 1) {
    iget_uint8(&iter, &packet->fuel);
  } else {
    packet->fuel=0;
  }

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/**************************************************************************
...
**************************************************************************/
struct packet_new_year *
receive_packet_new_year(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_new_year *packet=
    fc_malloc(sizeof(struct packet_new_year));

  pack_iter_init(&iter, pc);

  iget_uint32(&iter, &packet->year);

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}


/**************************************************************************
...
**************************************************************************/
int send_packet_move_unit(struct connection *pc, struct 
			  packet_move_unit *request)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;

  cptr=put_uint8(buffer+2, PACKET_MOVE_UNIT);
  cptr=put_uint8(cptr, request->x);
  cptr=put_uint8(cptr, request->y);
  cptr=put_uint16(cptr, request->unid);
  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}



/**************************************************************************
...
**************************************************************************/
struct packet_move_unit *receive_packet_move_unit(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_move_unit *packet=
    fc_malloc(sizeof(struct packet_move_unit));

  pack_iter_init(&iter, pc);

  iget_uint8(&iter, &packet->x);
  iget_uint8(&iter, &packet->y);
  iget_uint16(&iter, &packet->unid);

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/**************************************************************************
...
**************************************************************************/
int send_packet_req_join_game(struct connection *pc, struct 
			      packet_req_join_game *request)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, PACKET_REQUEST_JOIN_GAME);
  cptr=put_string(cptr, request->short_name);
  cptr=put_uint32(cptr, request->major_version);
  cptr=put_uint32(cptr, request->minor_version);
  cptr=put_uint32(cptr, request->patch_version);
  cptr=put_string(cptr, request->capability);
  cptr=put_string(cptr, request->name);
  cptr=put_string(cptr, request->version_label);
  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}

/**************************************************************************
  ...
  Fills in conn.id automatically, no need to set in packet_join_game_reply.
**************************************************************************/
int send_packet_join_game_reply(struct connection *pc, struct 
			        packet_join_game_reply *reply)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, PACKET_JOIN_GAME_REPLY);
  cptr=put_uint32(cptr, reply->you_can_join);
  /* if other end is byte swapped, you_can_join should be 0,
     which is 0 even if bytes are swapped! --dwp */
  cptr=put_string(cptr, reply->message);
  cptr=put_string(cptr, reply->capability);

if (pc && has_capability("conn_info", pc->capability)) {
  cptr=put_uint32(cptr, pc->id);
}
 
  /* so that old clients will understand the reply: */
  if(pc->byte_swap) {
    put_uint16(buffer, swab_uint16(cptr-buffer));
  } else {
    put_uint16(buffer, cptr-buffer);
  }

  return send_connection_data(pc, buffer, cptr-buffer);
}

/**************************************************************************
...
**************************************************************************/
int send_packet_generic_message(struct connection *pc, int type,
				struct packet_generic_message *packet)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, type);

  /* when removing "event00_fix" capability,
   leave only the code from the *else* clause (send unmodified values) */
if (packet->y < 0 && pc && !has_capability("event00_fix", pc->capability)) {
  cptr=put_uint8(cptr, 0);
  cptr=put_uint8(cptr, 0);
} else {
  cptr=put_uint8(cptr, packet->x);
  cptr=put_uint8(cptr, packet->y);
}
  /* when removing "event_wonder_obsolete" capability,
   leave only the code from the *else* clause (send unmodified value) */
if (packet->event == E_WONDER_OBSOLETE && pc
    && !has_capability("event_wonder_obsolete", pc->capability)) {
  cptr=put_uint32(cptr, E_NOEVENT);
} else {
  cptr=put_uint32(cptr, packet->event);
}

  cptr=put_string(cptr, packet->message);
  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}

/**************************************************************************
...
**************************************************************************/
int send_packet_generic_integer(struct connection *pc, int type,
				struct packet_generic_integer *packet)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, type);
  cptr=put_uint32(cptr, packet->value);
  put_uint16(buffer, cptr-buffer);
  return send_connection_data(pc, buffer, cptr-buffer);
}

/**************************************************************************
...
**************************************************************************/
struct packet_req_join_game *
receive_packet_req_join_game(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_req_join_game *packet=
    fc_malloc(sizeof(struct packet_req_join_game));

  pack_iter_init(&iter, pc);

  iget_string(&iter, packet->short_name, sizeof(packet->short_name));
  iget_uint32(&iter, &packet->major_version);
  iget_uint32(&iter, &packet->minor_version);
  iget_uint32(&iter, &packet->patch_version);
  iget_string(&iter, packet->capability, sizeof(packet->capability));
  if (pack_iter_remaining(&iter)) {
    iget_string(&iter, packet->name, sizeof(packet->name));
  } else {
    sz_strlcpy(packet->name, packet->short_name);
  }
  if (pack_iter_remaining(&iter)) {
    iget_string(&iter, packet->version_label, sizeof(packet->version_label));
  } else {
    packet->version_label[0] = '\0';
  }

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/**************************************************************************
...
**************************************************************************/
struct packet_join_game_reply *
receive_packet_join_game_reply(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_join_game_reply *packet=
    fc_malloc(sizeof(struct packet_join_game_reply));

  pack_iter_init(&iter, pc);

  iget_uint32(&iter, &packet->you_can_join);
  iget_string(&iter, packet->message, sizeof(packet->message));
  iget_string(&iter, packet->capability, sizeof(packet->capability));

  /* NOTE: pc doesn't yet have capability filled in!  Use packet value: */
if (has_capability("conn_info", packet->capability)) {
  iget_uint32(&iter, &packet->conn_id);
} else {
  packet->conn_id = 0;
}

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/**************************************************************************
...
**************************************************************************/
struct packet_generic_message *
receive_packet_generic_message(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_generic_message *packet=
    fc_malloc(sizeof(struct packet_generic_message));

  pack_iter_init(&iter, pc);

  iget_uint8(&iter, &packet->x);
  iget_uint8(&iter, &packet->y);
  
  /* when removing "event00_fix" capability, remove following block */
  if (packet->x==0 && packet->y==0
      && !has_capability("event00_fix", pc->capability)) {
    packet->x = -1;
    packet->y = -1;
  }
  /* remove to here */
 
  iget_uint32(&iter, &packet->event);
  iget_string(&iter, packet->message, sizeof(packet->message));
  
  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/**************************************************************************
...
**************************************************************************/
struct packet_generic_integer *
receive_packet_generic_integer(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_generic_integer *packet=
    fc_malloc(sizeof(struct packet_generic_integer));

  pack_iter_init(&iter, pc);

  iget_uint32(&iter, &packet->value);

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/**************************************************************************
...
**************************************************************************/
int send_packet_alloc_nation(struct connection *pc, 
			     struct packet_alloc_nation *packet)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, PACKET_ALLOC_NATION);
  cptr=put_uint32(cptr, packet->nation_no);
  cptr=put_string(cptr, packet->name);
  cptr=put_uint8(cptr,packet->is_male);
  cptr=put_uint8(cptr,packet->city_style);
  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}

/**************************************************************************
...
**************************************************************************/
struct packet_alloc_nation *
receive_packet_alloc_nation(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_alloc_nation *packet=
    fc_malloc(sizeof(struct packet_alloc_nation));

  pack_iter_init(&iter, pc);

  iget_uint32(&iter, &packet->nation_no);
  iget_string(&iter, packet->name, sizeof(packet->name));
  iget_uint8(&iter, &packet->is_male);
  iget_uint8(&iter, &packet->city_style);

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/**************************************************************************
...
**************************************************************************/
int send_packet_generic_values(struct connection *pc, int type,
			       struct packet_generic_values *req)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  
  cptr=put_uint8(buffer+2, type);
  cptr=put_uint16(cptr, req->id);
  cptr=put_uint32(cptr, req->value1);
  cptr=put_uint32(cptr, req->value2);

  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}

/**************************************************************************
...
**************************************************************************/
struct packet_generic_values *
receive_packet_generic_values(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_generic_values *packet=
    fc_malloc(sizeof(struct packet_generic_values));

  pack_iter_init(&iter, pc);

  iget_uint16(&iter, &packet->id);
  if (pack_iter_remaining(&iter) >= 4) {
    iget_uint32(&iter, &packet->value1);
  } else {
    packet->value1 = 0;
  }
  if (pack_iter_remaining(&iter) >= 4) {
    iget_uint32(&iter, &packet->value2);
  } else {
    packet->value2 = 0;
  }

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/*************************************************************************
...
**************************************************************************/
int send_packet_ruleset_control(struct connection *pc, 
				struct packet_ruleset_control *packet)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  
  cptr=put_uint8(buffer+2, PACKET_RULESET_CONTROL);
  
  cptr=put_uint8(cptr, packet->aqueduct_size);
  cptr=put_uint8(cptr, packet->sewer_size);
  cptr=put_uint8(cptr, packet->add_to_size_limit);

  cptr=put_uint8(cptr, packet->rtech.get_bonus_tech);
  cptr=put_uint8(cptr, packet->rtech.cathedral_plus);
  cptr=put_uint8(cptr, packet->rtech.cathedral_minus);
  cptr=put_uint8(cptr, packet->rtech.colosseum_plus);
  cptr=put_uint8(cptr, packet->rtech.temple_plus);

  cptr=put_uint8(cptr, packet->government_count);
  cptr=put_uint8(cptr, packet->default_government);
  cptr=put_uint8(cptr, packet->government_when_anarchy);
  
  cptr=put_uint8(cptr, packet->num_unit_types);
if (pc && has_capability("indef_impr_types", pc->capability)) {
  cptr=put_uint8(cptr, packet->num_impr_types);
}
  cptr=put_uint8(cptr, packet->num_tech_types);
 
  cptr=put_uint8(cptr, packet->nation_count);
  cptr=put_uint8(cptr, packet->playable_nation_count);
  cptr=put_uint8(cptr, packet->style_count);

  cptr=put_tech_list(cptr, packet->rtech.partisan_req);

  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}

/*************************************************************************
...
**************************************************************************/
struct packet_ruleset_control *
receive_packet_ruleset_control(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_ruleset_control *packet =
    fc_malloc(sizeof(struct packet_ruleset_control));

  pack_iter_init(&iter, pc);

  iget_uint8(&iter, &packet->aqueduct_size);
  iget_uint8(&iter, &packet->sewer_size);
  iget_uint8(&iter, &packet->add_to_size_limit);

  iget_uint8(&iter, &packet->rtech.get_bonus_tech);
  iget_uint8(&iter, &packet->rtech.cathedral_plus);
  iget_uint8(&iter, &packet->rtech.cathedral_minus);
  iget_uint8(&iter, &packet->rtech.colosseum_plus);
  iget_uint8(&iter, &packet->rtech.temple_plus);
  
  iget_uint8(&iter, &packet->government_count);
  iget_uint8(&iter, &packet->default_government);
  iget_uint8(&iter, &packet->government_when_anarchy);

  iget_uint8(&iter, &packet->num_unit_types);
if (pc && has_capability("indef_impr_types", pc->capability)) {
  iget_uint8(&iter, &packet->num_impr_types);
} else {
packet->num_impr_types = B_LAST_ENUM;
}
  iget_uint8(&iter, &packet->num_tech_types);

  iget_uint8(&iter, &packet->nation_count);
  iget_uint8(&iter, &packet->playable_nation_count);
  iget_uint8(&iter, &packet->style_count);

  iget_tech_list(&iter, packet->rtech.partisan_req);

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/**************************************************************************
...
**************************************************************************/
int send_packet_ruleset_unit(struct connection *pc,
			     struct packet_ruleset_unit *packet)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, PACKET_RULESET_UNIT);
  
  cptr=put_uint8(cptr, packet->id);
  cptr=put_uint8(cptr, packet->move_type);
  cptr=put_uint16(cptr, packet->build_cost);
  cptr=put_uint8(cptr, packet->attack_strength);
  cptr=put_uint8(cptr, packet->defense_strength);
  cptr=put_uint8(cptr, packet->move_rate);
  cptr=put_uint8(cptr, packet->tech_requirement);
  cptr=put_uint8(cptr, packet->vision_range);
  cptr=put_uint8(cptr, packet->transport_capacity);
  cptr=put_uint8(cptr, packet->hp);
  cptr=put_uint8(cptr, packet->firepower);
  cptr=put_uint8(cptr, packet->obsoleted_by);
  cptr=put_uint8(cptr, packet->fuel);
  cptr=put_uint32(cptr, packet->flags);
  cptr=put_uint32(cptr, packet->roles);
  cptr=put_uint8(cptr, packet->happy_cost);   /* unit upkeep -- SKi */
  cptr=put_uint8(cptr, packet->shield_cost);
  cptr=put_uint8(cptr, packet->food_cost);
  cptr=put_uint8(cptr, packet->gold_cost);
  cptr=put_string(cptr, packet->name);
  cptr=put_string(cptr, packet->graphic_str);
  cptr=put_string(cptr, packet->graphic_alt);
  if(unit_flag(packet->id, F_PARATROOPERS)) {
    cptr=put_uint16(cptr, packet->paratroopers_range);
    cptr=put_uint8(cptr, packet->paratroopers_mr_req);
    cptr=put_uint8(cptr, packet->paratroopers_mr_sub);
  }

  /* This must be last, so client can determine length: */
  if(packet->helptext) {
    cptr=put_string(cptr, packet->helptext);
  }
  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}

/**************************************************************************
...
**************************************************************************/
struct packet_ruleset_unit *
receive_packet_ruleset_unit(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_ruleset_unit *packet=
    fc_malloc(sizeof(struct packet_ruleset_unit));
  int len;

  pack_iter_init(&iter, pc);

  iget_uint8(&iter, &packet->id);
  iget_uint8(&iter, &packet->move_type);
  iget_uint16(&iter, &packet->build_cost);
  iget_uint8(&iter, &packet->attack_strength);
  iget_uint8(&iter, &packet->defense_strength);
  iget_uint8(&iter, &packet->move_rate);
  iget_uint8(&iter, &packet->tech_requirement);
  iget_uint8(&iter, &packet->vision_range);
  iget_uint8(&iter, &packet->transport_capacity);
  iget_uint8(&iter, &packet->hp);
  iget_uint8(&iter, &packet->firepower);
  iget_uint8(&iter, &packet->obsoleted_by);
  if (packet->obsoleted_by>127) packet->obsoleted_by-=256;
  iget_uint8(&iter, &packet->fuel);
  iget_uint32(&iter, &packet->flags);
  iget_uint32(&iter, &packet->roles);
  iget_uint8(&iter, &packet->happy_cost);   /* unit upkeep -- SKi */
  iget_uint8(&iter, &packet->shield_cost);
  iget_uint8(&iter, &packet->food_cost);
  iget_uint8(&iter, &packet->gold_cost);
  iget_string(&iter, packet->name, sizeof(packet->name));
  iget_string(&iter, packet->graphic_str, sizeof(packet->graphic_str));
  iget_string(&iter, packet->graphic_alt, sizeof(packet->graphic_alt));
  if(packet->flags & (1L<<F_PARATROOPERS)) {
    iget_uint16(&iter, &packet->paratroopers_range);
    iget_uint8(&iter, &packet->paratroopers_mr_req);
    iget_uint8(&iter, &packet->paratroopers_mr_sub);
  } else {
    packet->paratroopers_range=0;
    packet->paratroopers_mr_req=0;
    packet->paratroopers_mr_sub=0;
  }

  len = pack_iter_remaining(&iter);
  if (len) {
    packet->helptext = fc_malloc(len);
    iget_string(&iter, packet->helptext, len);
  } else {
    packet->helptext = NULL;
  }

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}


/**************************************************************************
...
**************************************************************************/
int send_packet_ruleset_tech(struct connection *pc,
			     struct packet_ruleset_tech *packet)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, PACKET_RULESET_TECH);
  
  cptr=put_uint8(cptr, packet->id);
  cptr=put_uint8(cptr, packet->req[0]);
  cptr=put_uint8(cptr, packet->req[1]);
  cptr=put_uint32(cptr, packet->flags);
  cptr=put_string(cptr, packet->name);
  
  /* This must be last, so client can determine length: */
  if(packet->helptext) {
    cptr=put_string(cptr, packet->helptext);
  }
  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}

/**************************************************************************
...
**************************************************************************/
struct packet_ruleset_tech *
receive_packet_ruleset_tech(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_ruleset_tech *packet=
    fc_malloc(sizeof(struct packet_ruleset_tech));
  int len;

  pack_iter_init(&iter, pc);

  iget_uint8(&iter, &packet->id);
  iget_uint8(&iter, &packet->req[0]);
  iget_uint8(&iter, &packet->req[1]);
  iget_uint32(&iter, &packet->flags);
  iget_string(&iter, packet->name, sizeof(packet->name));

  len = pack_iter_remaining(&iter);
  if (len) {
    packet->helptext = fc_malloc(len);
    iget_string(&iter, packet->helptext, len);
  } else {
    packet->helptext = NULL;
  }

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/**************************************************************************
...
**************************************************************************/
int send_packet_ruleset_building(struct connection *pc,
			     struct packet_ruleset_building *packet)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  struct impr_effect *eff;
  int count;

  cptr=put_uint8(buffer+2, PACKET_RULESET_BUILDING);

  cptr=put_uint8(cptr, packet->id);
  cptr=put_uint8(cptr, packet->tech_req);
  cptr=put_uint8(cptr, packet->bldg_req);
  cptr=put_uint8_vec8(cptr, (int *)packet->terr_gate, T_LAST);
  cptr=put_uint16_vec8(cptr, (int *)packet->spec_gate, S_NO_SPECIAL);
  cptr=put_uint8(cptr, packet->equiv_range);
  cptr=put_uint8_vec8(cptr, packet->equiv_dupl, B_LAST);
  cptr=put_uint8_vec8(cptr, packet->equiv_repl, B_LAST);
  cptr=put_uint8(cptr, packet->obsolete_by);
  cptr=put_uint8(cptr, packet->is_wonder);
  cptr=put_uint16(cptr, packet->build_cost);
  cptr=put_uint8(cptr, packet->upkeep);
  cptr=put_uint8(cptr, packet->sabotage);
  for (count = 0, eff = packet->effect; eff->type != EFT_LAST; count++, eff++) ;
  cptr=put_uint8(cptr, count);
  for (eff = packet->effect; eff->type != EFT_LAST; eff++) {
    cptr=put_uint8(cptr, eff->type);
    cptr=put_uint8(cptr, eff->range);
    cptr=put_sint16(cptr, eff->amount);
if (pc && has_capability("gen_impr_oversights", pc->capability)) {
    cptr=put_uint8(cptr, eff->survives);
}
    cptr=put_uint8(cptr, eff->cond_bldg);
    cptr=put_uint8(cptr, eff->cond_gov);
    cptr=put_uint8(cptr, eff->cond_adv);
    cptr=put_uint8(cptr, eff->cond_eff);
    cptr=put_uint8(cptr, eff->aff_unit);
    cptr=put_uint8(cptr, eff->aff_terr);
    cptr=put_uint16(cptr, eff->aff_spec);
  }
  cptr=put_uint8(cptr, packet->variant);	/* FIXME: remove when gen-impr obsoletes */
  cptr=put_string(cptr, packet->name);

  /* This must be last, so client can determine length: */
  if(packet->helptext) {
    cptr=put_string(cptr, packet->helptext);
  }
  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}

/**************************************************************************
...
**************************************************************************/
struct packet_ruleset_building *
receive_packet_ruleset_building(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_ruleset_building *packet=
    fc_malloc(sizeof(struct packet_ruleset_building));
  int len, inx, count;

  pack_iter_init(&iter, pc);

  iget_uint8(&iter, &packet->id);
  iget_uint8(&iter, &packet->tech_req);
  iget_uint8(&iter, &packet->bldg_req);
  iget_uint8_vec8(&iter, (int **)&packet->terr_gate, T_LAST);
  iget_uint16_vec8(&iter, (int **)&packet->spec_gate, S_NO_SPECIAL);
  iget_uint8(&iter, (int *)&packet->equiv_range);
  iget_uint8_vec8(&iter, &packet->equiv_dupl, B_LAST);
  iget_uint8_vec8(&iter, &packet->equiv_repl, B_LAST);
  iget_uint8(&iter, &packet->obsolete_by);
  iget_uint8(&iter, &packet->is_wonder);
  iget_uint16(&iter, &packet->build_cost);
  iget_uint8(&iter, &packet->upkeep);
  iget_uint8(&iter, &packet->sabotage);
  iget_uint8(&iter, &count);
  packet->effect = fc_malloc((count + 1) * sizeof(struct impr_effect));
  for (inx = 0; inx < count; inx++) {
    iget_uint8(&iter, (int *)&(packet->effect[inx].type));
    iget_uint8(&iter, (int *)&(packet->effect[inx].range));
    iget_sint16(&iter, &(packet->effect[inx].amount));
if (pc && has_capability("gen_impr_oversights", pc->capability)) {
    iget_uint8(&iter, &(packet->effect[inx].survives));
} else {
packet->effect[inx].survives =
  ((packet->effect[inx].type == EFT_ENABLE_NUKE) ||
   (packet->effect[inx].type == EFT_ENABLE_SPACE));
}
    iget_uint8(&iter, &(packet->effect[inx].cond_bldg));
    iget_uint8(&iter, &(packet->effect[inx].cond_gov));
    iget_uint8(&iter, &(packet->effect[inx].cond_adv));
    iget_uint8(&iter, (int *)&(packet->effect[inx].cond_eff));
    iget_uint8(&iter, (int *)&(packet->effect[inx].aff_unit));
    iget_uint8(&iter, (int *)&(packet->effect[inx].aff_terr));
    iget_uint16(&iter, (int *)&(packet->effect[inx].aff_spec));
  }
  packet->effect[count].type = EFT_LAST;
  iget_uint8(&iter, &packet->variant);	/* FIXME: remove when gen-impr obsoletes */
  iget_string(&iter, packet->name, sizeof(packet->name));

  len = pack_iter_remaining(&iter);
  if (len) {
    packet->helptext = fc_malloc(len);
    iget_string(&iter, packet->helptext, len);
  } else {
    packet->helptext = NULL;
  }

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/**************************************************************************
...
**************************************************************************/
int send_packet_ruleset_terrain(struct connection *pc,
				struct packet_ruleset_terrain *packet)
{
  int i;
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, PACKET_RULESET_TERRAIN);

  cptr=put_uint8(cptr, packet->id);
  cptr=put_string(cptr, packet->terrain_name);
  cptr=put_uint8(cptr, packet->movement_cost);
  cptr=put_uint8(cptr, packet->defense_bonus);
  cptr=put_uint8(cptr, packet->food);
  cptr=put_uint8(cptr, packet->shield);
  cptr=put_uint8(cptr, packet->trade);
  cptr=put_string(cptr, packet->special_1_name);
  cptr=put_uint8(cptr, packet->food_special_1);
  cptr=put_uint8(cptr, packet->shield_special_1);
  cptr=put_uint8(cptr, packet->trade_special_1);
  cptr=put_string(cptr, packet->special_2_name);
  cptr=put_uint8(cptr, packet->food_special_2);
  cptr=put_uint8(cptr, packet->shield_special_2);
  cptr=put_uint8(cptr, packet->trade_special_2);
  cptr=put_uint8(cptr, packet->road_trade_incr);
  cptr=put_uint8(cptr, packet->road_time);
  cptr=put_uint8(cptr, packet->irrigation_result);
  cptr=put_uint8(cptr, packet->irrigation_food_incr);
  cptr=put_uint8(cptr, packet->irrigation_time);
  cptr=put_uint8(cptr, packet->mining_result);
  cptr=put_uint8(cptr, packet->mining_shield_incr);
  cptr=put_uint8(cptr, packet->mining_time);
  cptr=put_uint8(cptr, packet->transform_result);
  cptr=put_uint8(cptr, packet->transform_time);
  cptr=put_string(cptr, packet->graphic_str);
  cptr=put_string(cptr, packet->graphic_alt);
  for(i=0; i<2; i++) {
    cptr=put_string(cptr, packet->special[i].graphic_str);
    cptr=put_string(cptr, packet->special[i].graphic_alt);
  }

  /* This must be last, so client can determine length: */
  if(packet->helptext) {
    cptr=put_string(cptr, packet->helptext);
  }
  
  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}

/**************************************************************************
...
**************************************************************************/
struct packet_ruleset_terrain *
receive_packet_ruleset_terrain(struct connection *pc)
{
  int i, len;
  struct pack_iter iter;
  struct packet_ruleset_terrain *packet=
    fc_malloc(sizeof(struct packet_ruleset_terrain));

  pack_iter_init(&iter, pc);

  iget_uint8(&iter, &packet->id);
  iget_string(&iter, packet->terrain_name, sizeof(packet->terrain_name));
  iget_uint8(&iter, &packet->movement_cost);
  iget_uint8(&iter, &packet->defense_bonus);
  iget_uint8(&iter, &packet->food);
  iget_uint8(&iter, &packet->shield);
  iget_uint8(&iter, &packet->trade);
  iget_string(&iter, packet->special_1_name, sizeof(packet->special_1_name));
  iget_uint8(&iter, &packet->food_special_1);
  iget_uint8(&iter, &packet->shield_special_1);
  iget_uint8(&iter, &packet->trade_special_1);
  iget_string(&iter, packet->special_2_name, sizeof(packet->special_2_name));
  iget_uint8(&iter, &packet->food_special_2);
  iget_uint8(&iter, &packet->shield_special_2);
  iget_uint8(&iter, &packet->trade_special_2);
  iget_uint8(&iter, &packet->road_trade_incr);
  iget_uint8(&iter, &packet->road_time);
  iget_uint8(&iter, (int*)&packet->irrigation_result);
  iget_uint8(&iter, &packet->irrigation_food_incr);
  iget_uint8(&iter, &packet->irrigation_time);
  iget_uint8(&iter, (int*)&packet->mining_result);
  iget_uint8(&iter, &packet->mining_shield_incr);
  iget_uint8(&iter, &packet->mining_time);
  iget_uint8(&iter, (int*)&packet->transform_result);
  iget_uint8(&iter, &packet->transform_time);
  
  iget_string(&iter, packet->graphic_str, sizeof(packet->graphic_str));
  iget_string(&iter, packet->graphic_alt, sizeof(packet->graphic_alt));
  for(i=0; i<2; i++) {
    iget_string(&iter, packet->special[i].graphic_str,
		sizeof(packet->special[i].graphic_str));
    iget_string(&iter, packet->special[i].graphic_alt,
		sizeof(packet->special[i].graphic_alt));
  }

  len = pack_iter_remaining(&iter);
  if (len) {
    packet->helptext = fc_malloc(len);
    iget_string(&iter, packet->helptext, len);
  } else {
    packet->helptext = NULL;
  }

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/**************************************************************************
...
**************************************************************************/
int send_packet_ruleset_terrain_control(struct connection *pc,
					struct terrain_misc *packet)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, PACKET_RULESET_TERRAIN_CONTROL);

  cptr=put_uint8(cptr, packet->river_style);
  cptr=put_uint8(cptr, packet->may_road);
  cptr=put_uint8(cptr, packet->may_irrigate);
  cptr=put_uint8(cptr, packet->may_mine);
  cptr=put_uint8(cptr, packet->may_transform);
  cptr=put_uint8(cptr, packet->ocean_reclaim_requirement);
if (pc && has_capability("land_channel_requirement", pc->capability)) {
  cptr=put_uint8(cptr, packet->land_channel_requirement);
}
  cptr=put_uint8(cptr, packet->river_move_mode);
  cptr=put_uint16(cptr, packet->river_defense_bonus);
  cptr=put_uint16(cptr, packet->river_trade_incr);
  cptr=put_uint16(cptr, packet->fortress_defense_bonus);
  cptr=put_uint16(cptr, packet->road_superhighway_trade_bonus);
  cptr=put_uint16(cptr, packet->rail_food_bonus);
  cptr=put_uint16(cptr, packet->rail_shield_bonus);
  cptr=put_uint16(cptr, packet->rail_trade_bonus);
  cptr=put_uint16(cptr, packet->farmland_supermarket_food_bonus);
  cptr=put_uint16(cptr, packet->pollution_food_penalty);
  cptr=put_uint16(cptr, packet->pollution_shield_penalty);
  cptr=put_uint16(cptr, packet->pollution_trade_penalty);
if (pc && has_capability("nuclear_fallout", pc->capability)) {
  cptr=put_uint16(cptr, packet->fallout_food_penalty);
  cptr=put_uint16(cptr, packet->fallout_shield_penalty);
  cptr=put_uint16(cptr, packet->fallout_trade_penalty);
}
  if (packet->river_help_text) cptr=put_string(cptr, packet->river_help_text);

  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}

/**************************************************************************
...
**************************************************************************/
struct terrain_misc *
receive_packet_ruleset_terrain_control(struct connection *pc)
{
  struct pack_iter iter;
  struct terrain_misc *packet=
    fc_malloc(sizeof(struct terrain_misc));
  int len;

  pack_iter_init(&iter, pc);

  iget_uint8(&iter, (int*)&packet->river_style);
  iget_uint8(&iter, &packet->may_road);
  iget_uint8(&iter, &packet->may_irrigate);
  iget_uint8(&iter, &packet->may_mine);
  iget_uint8(&iter, &packet->may_transform);
  iget_uint8(&iter, (int*)&packet->ocean_reclaim_requirement);
if (pc && has_capability("land_channel_requirement", pc->capability)) {
  iget_uint8(&iter, (int*)&packet->land_channel_requirement);
} else {
  packet->land_channel_requirement = 0;
}
  iget_uint8(&iter, (int*)&packet->river_move_mode);
  iget_uint16(&iter, &packet->river_defense_bonus);
  iget_uint16(&iter, &packet->river_trade_incr);
  iget_uint16(&iter, &packet->fortress_defense_bonus);
  iget_uint16(&iter, &packet->road_superhighway_trade_bonus);
  iget_uint16(&iter, &packet->rail_food_bonus);
  iget_uint16(&iter, &packet->rail_shield_bonus);
  iget_uint16(&iter, &packet->rail_trade_bonus);
  iget_uint16(&iter, &packet->farmland_supermarket_food_bonus);
  iget_uint16(&iter, &packet->pollution_food_penalty);
  iget_uint16(&iter, &packet->pollution_shield_penalty);
  iget_uint16(&iter, &packet->pollution_trade_penalty);
if (pc && has_capability("nuclear_fallout", pc->capability)) {
  iget_uint16(&iter, &packet->fallout_food_penalty);
  iget_uint16(&iter, &packet->fallout_shield_penalty);
  iget_uint16(&iter, &packet->fallout_trade_penalty);
}

  len = pack_iter_remaining(&iter);
  if (len) {
    packet->river_help_text = fc_malloc(len);
    iget_string(&iter, packet->river_help_text, len);
  } else {
    packet->river_help_text = NULL;
  }

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/**************************************************************************
...
**************************************************************************/
int send_packet_ruleset_government(struct connection *pc,
			     struct packet_ruleset_government *packet)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, PACKET_RULESET_GOVERNMENT);
  
  cptr=put_uint8(cptr, packet->id);
  
  cptr=put_uint8(cptr, packet->required_tech);
  cptr=put_uint8(cptr, packet->max_rate);
  cptr=put_uint8(cptr, packet->civil_war);
  cptr=put_uint8(cptr, packet->martial_law_max);
  cptr=put_uint8(cptr, packet->martial_law_per);
  cptr=put_uint8(cptr, packet->empire_size_mod);
  cptr=put_uint8(cptr, packet->empire_size_inc);
  cptr=put_uint8(cptr, packet->rapture_size);
  
  cptr=put_uint8(cptr, packet->unit_happy_cost_factor);
  cptr=put_uint8(cptr, packet->unit_shield_cost_factor);
  cptr=put_uint8(cptr, packet->unit_food_cost_factor);
  cptr=put_uint8(cptr, packet->unit_gold_cost_factor);
  
  cptr=put_uint8(cptr, packet->free_happy);
  cptr=put_uint8(cptr, packet->free_shield);
  cptr=put_uint8(cptr, packet->free_food);
  cptr=put_uint8(cptr, packet->free_gold);

  cptr=put_uint8(cptr, packet->trade_before_penalty);
  cptr=put_uint8(cptr, packet->shields_before_penalty);
  cptr=put_uint8(cptr, packet->food_before_penalty);

  cptr=put_uint8(cptr, packet->celeb_trade_before_penalty);
  cptr=put_uint8(cptr, packet->celeb_shields_before_penalty);
  cptr=put_uint8(cptr, packet->celeb_food_before_penalty);

  cptr=put_uint8(cptr, packet->trade_bonus);
  cptr=put_uint8(cptr, packet->shield_bonus);
  cptr=put_uint8(cptr, packet->food_bonus);

  cptr=put_uint8(cptr, packet->celeb_trade_bonus);
  cptr=put_uint8(cptr, packet->celeb_shield_bonus);
  cptr=put_uint8(cptr, packet->celeb_food_bonus);

  cptr=put_uint8(cptr, packet->corruption_level);
  cptr=put_uint8(cptr, packet->corruption_modifier);
  cptr=put_uint8(cptr, packet->fixed_corruption_distance);
  cptr=put_uint8(cptr, packet->corruption_distance_factor);
  cptr=put_uint8(cptr, packet->extra_corruption_distance);

  cptr=put_uint8(cptr, packet->flags);
  cptr=put_uint8(cptr, packet->hints);

  cptr=put_uint8(cptr, packet->num_ruler_titles);

  cptr=put_string(cptr, packet->name);
  cptr=put_string(cptr, packet->graphic_str);
  cptr=put_string(cptr, packet->graphic_alt);

  /* This must be last, so client can determine length: */
  if(packet->helptext) {
    cptr=put_string(cptr, packet->helptext);
  }
  
  freelog(LOG_DEBUG, "send gov %s", packet->name);

  put_uint16(buffer, cptr-buffer);
  return send_connection_data(pc, buffer, cptr-buffer);
}
int send_packet_ruleset_government_ruler_title(struct connection *pc,
			     struct packet_ruleset_government_ruler_title *packet)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, PACKET_RULESET_GOVERNMENT_RULER_TITLE);
  
  cptr=put_uint8(cptr, packet->gov);
  cptr=put_uint8(cptr, packet->id);
  cptr=put_uint8(cptr, packet->nation);

  cptr=put_string(cptr, packet->male_title);
  cptr=put_string(cptr, packet->female_title);
  
  put_uint16(buffer, cptr-buffer);
  return send_connection_data(pc, buffer, cptr-buffer);
}

/**************************************************************************
...
**************************************************************************/
struct packet_ruleset_government *
receive_packet_ruleset_government(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_ruleset_government *packet=
    fc_calloc(1, sizeof(struct packet_ruleset_government));
  int len;

  pack_iter_init(&iter, pc);

  iget_uint8(&iter, &packet->id);
  
  iget_uint8(&iter, &packet->required_tech);
  iget_uint8(&iter, &packet->max_rate);
  iget_uint8(&iter, &packet->civil_war);
  iget_uint8(&iter, &packet->martial_law_max);
  iget_uint8(&iter, &packet->martial_law_per);
  iget_uint8(&iter, &packet->empire_size_mod);
  if(packet->empire_size_mod > 127) packet->empire_size_mod-=256;
  iget_uint8(&iter, &packet->empire_size_inc);
  iget_uint8(&iter, &packet->rapture_size);
  
  iget_uint8(&iter, &packet->unit_happy_cost_factor);
  iget_uint8(&iter, &packet->unit_shield_cost_factor);
  iget_uint8(&iter, &packet->unit_food_cost_factor);
  iget_uint8(&iter, &packet->unit_gold_cost_factor);
  
  iget_uint8(&iter, &packet->free_happy);
  iget_uint8(&iter, &packet->free_shield);
  iget_uint8(&iter, &packet->free_food);
  iget_uint8(&iter, &packet->free_gold);

  iget_uint8(&iter, &packet->trade_before_penalty);
  iget_uint8(&iter, &packet->shields_before_penalty);
  iget_uint8(&iter, &packet->food_before_penalty);

  iget_uint8(&iter, &packet->celeb_trade_before_penalty);
  iget_uint8(&iter, &packet->celeb_shields_before_penalty);
  iget_uint8(&iter, &packet->celeb_food_before_penalty);

  iget_uint8(&iter, &packet->trade_bonus);
  iget_uint8(&iter, &packet->shield_bonus);
  iget_uint8(&iter, &packet->food_bonus);

  iget_uint8(&iter, &packet->celeb_trade_bonus);
  iget_uint8(&iter, &packet->celeb_shield_bonus);
  iget_uint8(&iter, &packet->celeb_food_bonus);

  iget_uint8(&iter, &packet->corruption_level);
  iget_uint8(&iter, &packet->corruption_modifier);
  iget_uint8(&iter, &packet->fixed_corruption_distance);
  iget_uint8(&iter, &packet->corruption_distance_factor);
  iget_uint8(&iter, &packet->extra_corruption_distance);

  iget_uint8(&iter, &packet->flags);
  iget_uint8(&iter, &packet->hints);

  iget_uint8(&iter, &packet->num_ruler_titles);

  iget_string(&iter, packet->name, sizeof(packet->name));
  iget_string(&iter, packet->graphic_str, sizeof(packet->graphic_str));
  iget_string(&iter, packet->graphic_alt, sizeof(packet->graphic_alt));
  
  len = pack_iter_remaining(&iter);
  if (len) {
    packet->helptext = fc_malloc(len);
    iget_string(&iter, packet->helptext, len);
  } else {
    packet->helptext = NULL;
  }

  freelog(LOG_DEBUG, "recv gov %s", packet->name);

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}
struct packet_ruleset_government_ruler_title *
receive_packet_ruleset_government_ruler_title(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_ruleset_government_ruler_title *packet=
    fc_malloc(sizeof(struct packet_ruleset_government_ruler_title));

  pack_iter_init(&iter, pc);

  iget_uint8(&iter, &packet->gov);
  iget_uint8(&iter, &packet->id);
  iget_uint8(&iter, &packet->nation);

  iget_string(&iter, packet->male_title, sizeof(packet->male_title));
  iget_string(&iter, packet->female_title, sizeof(packet->female_title));

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/**************************************************************************
...
**************************************************************************/
int send_packet_ruleset_nation(struct connection *pc,
			       struct packet_ruleset_nation *packet)
{
  int i;
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, PACKET_RULESET_NATION);

  cptr=put_uint8(cptr, packet->id);

  cptr=put_string(cptr, packet->name);
  cptr=put_string(cptr, packet->name_plural);
  cptr=put_string(cptr, packet->graphic_str);
  cptr=put_string(cptr, packet->graphic_alt);
  cptr=put_uint8(cptr, packet->leader_count);
  for( i=0; i<packet->leader_count; i++ ) {
    cptr=put_string(cptr, packet->leader_name[i]);
    cptr=put_uint8(cptr, packet->leader_sex[i]);
  }
  cptr=put_uint8(cptr, packet->city_style);

  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}


/**************************************************************************
...
**************************************************************************/
struct packet_ruleset_nation *
receive_packet_ruleset_nation(struct connection *pc)
{
  int i;
  struct pack_iter iter;
  struct packet_ruleset_nation *packet=
    fc_malloc(sizeof(struct packet_ruleset_nation));

  pack_iter_init(&iter, pc);

  iget_uint8(&iter, &packet->id);
  iget_string(&iter, packet->name, sizeof(packet->name));
  iget_string(&iter, packet->name_plural, sizeof(packet->name_plural));
  iget_string(&iter, packet->graphic_str, sizeof(packet->graphic_str));
  iget_string(&iter, packet->graphic_alt, sizeof(packet->graphic_alt));
  iget_uint8(&iter, &packet->leader_count);
  for( i=0; i<packet->leader_count; i++ ) {
    iget_string(&iter, packet->leader_name[i], sizeof(packet->leader_name[i]));
    iget_uint8(&iter, &packet->leader_sex[i]);
  }
  iget_uint8(&iter, &packet->city_style);

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/**************************************************************************
...
**************************************************************************/
int send_packet_ruleset_city(struct connection *pc,
                             struct packet_ruleset_city *packet)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, PACKET_RULESET_CITY);

  cptr=put_uint8(cptr, packet->style_id);
  cptr=put_uint8(cptr, packet->techreq);
  cptr=put_sint16(cptr, packet->replaced_by);           /* I may send -1 */

  cptr=put_string(cptr, packet->name);
  cptr=put_string(cptr, packet->graphic);
  cptr=put_string(cptr, packet->graphic_alt);

  put_uint16(buffer, cptr-buffer);

  return send_connection_data(pc, buffer, cptr-buffer);
}

/**************************************************************************
...
**************************************************************************/
struct packet_ruleset_city *
receive_packet_ruleset_city(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_ruleset_city *packet=
    fc_malloc(sizeof(struct packet_ruleset_city));

  pack_iter_init(&iter, pc);

  iget_uint8(&iter, &packet->style_id);
  iget_uint8(&iter, &packet->techreq);
  iget_sint16(&iter, &packet->replaced_by);           /* may be -1 */

  iget_string(&iter, packet->name, MAX_LEN_NAME);
  iget_string(&iter, packet->graphic, MAX_LEN_NAME);
  iget_string(&iter, packet->graphic_alt, MAX_LEN_NAME);

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/**************************************************************************
...
**************************************************************************/
int send_packet_ruleset_game(struct connection *pc,
                             struct packet_ruleset_game *packet)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
/* when removing "game_ruleset" capability,
remove this *entire* piece of code (to REMOVE TO HERE, below) */
if (pc && !has_capability("game_ruleset", pc->capability)) {
return 0;
}
/* REMOVE TO HERE */
  cptr=put_uint8(buffer+2, PACKET_RULESET_GAME);

  cptr=put_uint8(cptr, packet->min_city_center_food);
  cptr=put_uint8(cptr, packet->min_city_center_shield);
  cptr=put_uint8(cptr, packet->min_city_center_trade);
  cptr=put_uint8(cptr, packet->min_dist_bw_cities);
  cptr=put_uint8(cptr, packet->init_vis_radius_sq);
  cptr=put_uint8(cptr, packet->hut_overflight);
  cptr=put_uint8(cptr, packet->pillage_select);
if (pc && has_capability("nuclear_fallout", pc->capability)) {
  cptr=put_uint8(cptr, packet->nuke_contamination);
}

  put_uint16(buffer, cptr-buffer);
  return send_connection_data(pc, buffer, cptr-buffer);
}

/**************************************************************************
...
**************************************************************************/
struct packet_ruleset_game *
receive_packet_ruleset_game(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_ruleset_game *packet=
    fc_malloc(sizeof(struct packet_ruleset_game));

  pack_iter_init(&iter, pc);

  iget_uint8(&iter, &packet->min_city_center_food);
  iget_uint8(&iter, &packet->min_city_center_shield);
  iget_uint8(&iter, &packet->min_city_center_trade);
  iget_uint8(&iter, &packet->min_dist_bw_cities);
  iget_uint8(&iter, &packet->init_vis_radius_sq);
  iget_uint8(&iter, &packet->hut_overflight);
  iget_uint8(&iter, &packet->pillage_select);
if (pc && has_capability("nuclear_fallout", pc->capability)) {
  iget_uint8(&iter, &packet->nuke_contamination);
} else {
  packet->nuke_contamination = CONTAMINATION_POLLUTION;
}

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/**************************************************************************
...
**************************************************************************/
int send_packet_spaceship_info(struct connection *pc,
			       struct packet_spaceship_info *packet)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, PACKET_SPACESHIP_INFO);
  
  cptr=put_uint8(cptr, packet->player_num);
  cptr=put_uint8(cptr, packet->sship_state);
  cptr=put_uint8(cptr, packet->structurals);
  cptr=put_uint8(cptr, packet->components);
  cptr=put_uint8(cptr, packet->modules);
  cptr=put_uint8(cptr, packet->fuel);
  cptr=put_uint8(cptr, packet->propulsion);
  cptr=put_uint8(cptr, packet->habitation);
  cptr=put_uint8(cptr, packet->life_support);
  cptr=put_uint8(cptr, packet->solar_panels);
  cptr=put_uint16(cptr, packet->launch_year);
  cptr=put_uint8(cptr, (packet->population/1000));
  cptr=put_uint32(cptr, packet->mass);
  cptr=put_uint32(cptr, (int) (packet->support_rate*10000));
  cptr=put_uint32(cptr, (int) (packet->energy_rate*10000));
  cptr=put_uint32(cptr, (int) (packet->success_rate*10000));
  cptr=put_uint32(cptr, (int) (packet->travel_time*10000));
  cptr=put_bit_string(cptr, (char*)packet->structure);

  put_uint16(buffer, cptr-buffer);
  return send_connection_data(pc, buffer, cptr-buffer);
}

/**************************************************************************
...
**************************************************************************/
struct packet_spaceship_info *
receive_packet_spaceship_info(struct connection *pc)
{
  int tmp;
  struct pack_iter iter;
  struct packet_spaceship_info *packet=
    fc_malloc(sizeof(struct packet_spaceship_info));
  
  pack_iter_init(&iter, pc);

  iget_uint8(&iter, &packet->player_num);
  iget_uint8(&iter, &packet->sship_state);
  iget_uint8(&iter, &packet->structurals);
  iget_uint8(&iter, &packet->components);
  iget_uint8(&iter, &packet->modules);
  iget_uint8(&iter, &packet->fuel);
  iget_uint8(&iter, &packet->propulsion);
  iget_uint8(&iter, &packet->habitation);
  iget_uint8(&iter, &packet->life_support);
  iget_uint8(&iter, &packet->solar_panels);
  iget_uint16(&iter, &packet->launch_year);
  
  if(packet->launch_year > 32767) packet->launch_year-=65536;
  
  iget_uint8(&iter, &packet->population);
  packet->population *= 1000;
  iget_uint32(&iter, &packet->mass);
  
  iget_uint32(&iter, &tmp);
  packet->support_rate = tmp * 0.0001;
  iget_uint32(&iter, &tmp);
  packet->energy_rate = tmp * 0.0001;
  iget_uint32(&iter, &tmp);
  packet->success_rate = tmp * 0.0001;
  iget_uint32(&iter, &tmp);
  packet->travel_time = tmp * 0.0001;

  iget_bit_string(&iter, (char*)packet->structure, sizeof(packet->structure));
  
  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/**************************************************************************
...
**************************************************************************/
int send_packet_spaceship_action(struct connection *pc,
				 struct packet_spaceship_action *packet)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, PACKET_SPACESHIP_ACTION);
  
  cptr=put_uint8(cptr, packet->action);
  cptr=put_uint8(cptr, packet->num);

  put_uint16(buffer, cptr-buffer);
  return send_connection_data(pc, buffer, cptr-buffer);
}

/**************************************************************************
...
**************************************************************************/
struct packet_spaceship_action *
receive_packet_spaceship_action(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_spaceship_action *packet=
    fc_malloc(sizeof(struct packet_spaceship_action));
  
  pack_iter_init(&iter, pc);

  iget_uint8(&iter, &packet->action);
  iget_uint8(&iter, &packet->num);

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/**************************************************************************
...
**************************************************************************/
int send_packet_city_name_suggestion(struct connection *pc,
				     struct packet_city_name_suggestion *packet)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr=put_uint8(buffer+2, PACKET_CITY_NAME_SUGGESTION);
  
  cptr=put_uint16(cptr, packet->id);
  cptr=put_string(cptr, packet->name);

  put_uint16(buffer, cptr-buffer);
  return send_connection_data(pc, buffer, cptr-buffer);
}

/**************************************************************************
...
**************************************************************************/
struct packet_city_name_suggestion *
receive_packet_city_name_suggestion(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_city_name_suggestion *packet=
    fc_malloc(sizeof(struct packet_city_name_suggestion));
  
  pack_iter_init(&iter, pc);

  iget_uint16(&iter, &packet->id);
  iget_string(&iter, packet->name, sizeof(packet->name));

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}

/**************************************************************************
...
**************************************************************************/
int send_packet_sabotage_list(struct connection *pc,
			      struct packet_sabotage_list *packet)
{
  unsigned char buffer[MAX_LEN_PACKET], *cptr;
  cptr = put_uint8(buffer+2, PACKET_SABOTAGE_LIST);

  cptr = put_uint16(cptr, packet->diplomat_id);
  cptr = put_uint16(cptr, packet->city_id);
  cptr = put_bit_string(cptr, (char *)packet->improvements);

  put_uint16(buffer, cptr-buffer);
  return send_connection_data(pc, buffer, cptr-buffer);
}

/**************************************************************************
...
**************************************************************************/
struct packet_sabotage_list *
receive_packet_sabotage_list(struct connection *pc)
{
  struct pack_iter iter;
  struct packet_sabotage_list *packet=
    fc_malloc(sizeof(struct packet_sabotage_list));

  pack_iter_init(&iter, pc);

  iget_uint16(&iter, &packet->diplomat_id);
  iget_uint16(&iter, &packet->city_id);
  iget_bit_string(&iter, (char*)packet->improvements,
		  sizeof(packet->improvements));

  pack_iter_end(&iter, pc);
  remove_packet_from_buffer(pc->buffer);
  return packet;
}
