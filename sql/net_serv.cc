/* Copyright (c) 2000, 2016, Oracle and/or its affiliates.
   Copyright (c) 2012, 2022, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

/**
  @file

  This file is the net layer API for the MySQL client/server protocol.

  Write and read of logical packets to/from socket.

  Writes are cached into net_buffer_length big packets.
  Read packets are reallocated dynamicly when reading big packets.
  Each logical packet has the following pre-info:
  3 byte length & 1 byte package-number.

  This file needs to be written in C as it's used by the libmysql client as a
  C file.
*/

/*
  HFTODO this must be hidden if we don't want client capabilities in 
  embedded library
 */

#include "mariadb.h"
#include <mysql.h>
#include <mysql_com.h>
#include <mysqld_error.h>
#include <my_sys.h>
#include <m_string.h>
#include <my_net.h>
#include <violite.h>
#include <signal.h>
#include "probes_mysql.h"
#include <debug_sync.h>
#include "proxy_protocol.h"
#include <mysql_com_server.h>
#include <mysqld.h>

PSI_memory_key key_memory_NET_buff;
PSI_memory_key key_memory_NET_compress_packet;

#ifdef EMBEDDED_LIBRARY
#undef MYSQL_SERVER
#undef MYSQL_CLIENT
#define MYSQL_CLIENT
#endif /*EMBEDDED_LIBRARY */

/*
  to reduce the number of ifdef's in the code
*/
#ifdef EXTRA_DEBUG
#define EXTRA_DEBUG_fprintf fprintf
#define EXTRA_DEBUG_ASSERT DBUG_ASSERT
#else
static void inline EXTRA_DEBUG_fprintf(...) {}
#endif /* EXTRA_DEBUG */

#ifdef MYSQL_SERVER
#include <sql_class.h>
#include <sql_connect.h>

static void inline MYSQL_SERVER_my_error(uint error, myf flags)
{
  my_error(error,
           flags | MYF(global_system_variables.log_warnings > 3 ? ME_ERROR_LOG : 0));
}

#else
static void inline MYSQL_SERVER_my_error(...) {}
#endif

#ifndef EXTRA_DEBUG_ASSERT
# define EXTRA_DEBUG_ASSERT(X) do {} while(0)
#endif

/*
  The following handles the differences when this is linked between the
  client and the server.

  This gives an error if a too big packet is found.
  The server can change this, but because the client can't normally do this
  the client should have a bigger max_allowed_packet.
*/

#ifdef MYSQL_SERVER
/*
  The following variables/functions should really not be declared
  extern, but as it's hard to include sql_priv.h here, we have to
  live with this for a while.
*/
extern ulonglong test_flags;
extern ulong bytes_sent, bytes_received, net_big_packet_count;
#define USE_QUERY_CACHE
extern void query_cache_insert(void *thd, const char *packet, size_t length,
                               unsigned pkt_nr);
#define update_statistics(A) A
extern my_bool thd_net_is_killed(THD *thd);
/* Additional instrumentation hooks for the server */
#include "mysql_com_server.h"
#else
#define update_statistics(A)
#define thd_net_is_killed(A) 0
#endif


static my_bool net_write_buff(NET *, const uchar *, size_t len);

my_bool net_allocate_new_packet(NET *net, void *thd, uint my_flags);

/** Init with packet info. */

my_bool my_net_init(NET *net, Vio *vio, void *thd, uint my_flags)
{
  DBUG_ENTER("my_net_init");
  DBUG_PRINT("enter", ("my_flags: %u", my_flags));
  net->vio = vio;
  net->read_timeout= 0;
  net->write_timeout= 0;
  my_net_local_init(net);			/* Set some limits */

  if (net_allocate_new_packet(net, thd, my_flags))
    DBUG_RETURN(1);

  net->error=0; net->return_status=0;
  net->pkt_nr=net->compress_pkt_nr=0;
  net->last_error[0]=0;
  net->compress=0; net->reading_or_writing=0;
  net->where_b = net->remain_in_buf=0;
  net->net_skip_rest_factor= 0;
  net->last_errno=0;
  net->pkt_nr_can_be_reset= 0;
  net->using_proxy_protocol= 0;
  net->thread_specific_malloc= MY_TEST(my_flags & MY_THREAD_SPECIFIC);
  net->thd= 0;
  net->extension= NULL;
  net->thd= thd;

  if (vio)
  {
    /* For perl DBI/DBD. */
    net->fd= vio_fd(vio);
    if (!(test_flags & TEST_BLOCKING))
    {
      my_bool old_mode;
      vio_blocking(vio, FALSE, &old_mode);
    }
    vio_fastsend(vio);
  }
  DBUG_RETURN(0);
}


/**
  Allocate and assign new net buffer

  @note In case of error the old buffer left

  @retval TRUE error
  @retval FALSE success
*/

my_bool net_allocate_new_packet(NET *net, void *thd, uint my_flags)
{
  uchar *tmp;
  DBUG_ENTER("net_allocate_new_packet");
  if (!(tmp= (uchar*) my_malloc(key_memory_NET_buff,
                                (size_t) net->max_packet +
				NET_HEADER_SIZE + COMP_HEADER_SIZE + 1,
				MYF(MY_WME | my_flags))))
    DBUG_RETURN(1);
  net->buff= tmp;
  net->buff_end=net->buff+net->max_packet;
  net->write_pos=net->read_pos = net->buff;
  DBUG_RETURN(0);
}


void net_end(NET *net)
{
  DBUG_ENTER("net_end");
  my_free(net->buff);
  net->buff=0;
  net->using_proxy_protocol= 0;
  DBUG_VOID_RETURN;
}


/** Realloc the packet buffer. */

my_bool net_realloc(NET *net, size_t length)
{
  uchar *buff;
  size_t pkt_length;
  DBUG_ENTER("net_realloc");
  DBUG_PRINT("enter",("length: %lu", (ulong) length));

  if (length >= net->max_packet_size)
  {
    DBUG_PRINT("error", ("Packet too large. Max size: %lu",
                         net->max_packet_size));
    /* @todo: 1 and 2 codes are identical. */
    net->error= 1;
    net->last_errno= ER_NET_PACKET_TOO_LARGE;
    MYSQL_SERVER_my_error(ER_NET_PACKET_TOO_LARGE, MYF(0));
    DBUG_RETURN(1);
  }
  pkt_length = (length+IO_SIZE-1) & ~(IO_SIZE-1); 
  /*
    We must allocate some extra bytes for the end 0 and to be able to
    read big compressed blocks + 1 safety byte since uint3korr() in
    my_real_read() may actually read 4 bytes depending on build flags and
    platform.
  */
  if (!(buff= (uchar*) my_realloc(key_memory_NET_buff,
                                  (char*) net->buff, pkt_length +
                                  NET_HEADER_SIZE + COMP_HEADER_SIZE + 1,
                                  MYF(MY_WME | (net->thread_specific_malloc
                                                ?  MY_THREAD_SPECIFIC : 0)))))
  {
    /* @todo: 1 and 2 codes are identical. */
    net->error= 1;
    net->last_errno= ER_OUT_OF_RESOURCES;
    /* In the server the error is reported by MY_WME flag. */
    DBUG_RETURN(1);
  }
  net->buff=net->write_pos=buff;
  net->buff_end=buff+(net->max_packet= (ulong) pkt_length);
  DBUG_RETURN(0);
}


/**
  Check if there is any data to be read from the socket.

  @param sd   socket descriptor

  @retval
    0  No data to read
  @retval
    1  Data or EOF to read
  @retval
    -1   Don't know if data is ready or not
*/

#if !defined(EMBEDDED_LIBRARY) && defined(DBUG_OFF)

static int net_data_is_ready(my_socket sd)
{
#ifdef HAVE_POLL
  struct pollfd ufds;
  int res;

  ufds.fd= sd;
  ufds.events= POLLIN | POLLPRI;
  if (!(res= poll(&ufds, 1, 0)))
    return 0;
  if (res < 0 || !(ufds.revents & (POLLIN | POLLPRI)))
    return 0;
  return 1;
#else
  fd_set sfds;
  struct timeval tv;
  int res;

#ifndef _WIN32
  /* Windows uses an _array_ of 64 fd's as default, so it's safe */
  if (sd >= FD_SETSIZE)
    return -1;
#define NET_DATA_IS_READY_CAN_RETURN_MINUS_ONE
#endif

  FD_ZERO(&sfds);
  FD_SET(sd, &sfds);

  tv.tv_sec= tv.tv_usec= 0;

  if ((res= select((int) (sd + 1), &sfds, NULL, NULL, &tv)) < 0)
    return 0;
  else
    return MY_TEST(res ? FD_ISSET(sd, &sfds) : 0);
#endif /* HAVE_POLL */
}

#endif /* EMBEDDED_LIBRARY */

/**
  Clear (reinitialize) the NET structure for a new command.

  @remark Performs debug checking of the socket buffer to
          ensure that the protocol sequence is correct.

   - Read from socket until there is nothing more to read. Discard
     what is read.
   - Initialize net for new net_read/net_write calls.

   If there is anything when to read 'net_clear' is called this
   normally indicates an error in the protocol. Normally one should not
   need to do clear the communication buffer. If one compiles without
   -DUSE_NET_CLEAR then one wins one read call / query.

   When connection is properly closed (for TCP it means with
   a FIN packet), then select() considers a socket "ready to read",
   in the sense that there's EOF to read, but read() returns 0.

  @param net			NET handler
  @param clear_buffer           if <> 0, then clear all data from comm buff
*/

void net_clear(NET *net, my_bool clear_buffer __attribute__((unused)))
{
  DBUG_ENTER("net_clear");

/*
  We don't do a clear in case of not DBUG_OFF to catch bugs in the
  protocol handling.
*/

#if (!defined(EMBEDDED_LIBRARY) && defined(DBUG_OFF)) || defined(USE_NET_CLEAR)
  if (clear_buffer)
  {
    size_t count;
    int ready;
    while ((ready= net_data_is_ready(vio_fd(net->vio))) > 0)
    {
      /* The socket is ready */
      if ((long) (count= vio_read(net->vio, net->buff,
                                  (size_t) net->max_packet)) > 0)
      {
        DBUG_PRINT("info",("skipped %ld bytes from file: %s",
                           (long) count, vio_description(net->vio)));
        EXTRA_DEBUG_fprintf(stderr,"Note: net_clear() skipped %ld bytes from file: %s\n",
                (long) count, vio_description(net->vio));
      }
      else
      {
        DBUG_PRINT("info",("socket ready but only EOF to read - disconnected"));
        net->error= 2;
        break;
      }
    }
#ifdef NET_DATA_IS_READY_CAN_RETURN_MINUS_ONE
    /* 'net_data_is_ready' returned "don't know" */
    if (ready == -1)
    {
      /* Read unblocking to clear net */
      my_bool old_mode;
      if (!vio_blocking(net->vio, FALSE, &old_mode))
      {
        while ((long) (count= vio_read(net->vio, net->buff,
                                       (size_t) net->max_packet)) > 0)
          DBUG_PRINT("info",("skipped %ld bytes from file: %s",
                             (long) count, vio_description(net->vio)));
        vio_blocking(net->vio, TRUE, &old_mode);
      }
    }
#endif /* NET_DATA_IS_READY_CAN_RETURN_MINUS_ONE */
  }
#endif /* EMBEDDED_LIBRARY */
  net->pkt_nr=net->compress_pkt_nr=0;		/* Ready for new command */
  net->write_pos=net->buff;
  DBUG_VOID_RETURN;
}


/** Flush write_buffer if not empty. */

my_bool net_flush(NET *net)
{
  my_bool error= 0;
  DBUG_ENTER("net_flush");
  if (net->buff != net->write_pos)
  {
    error= MY_TEST(net_real_write(net, net->buff,
                                  (size_t) (net->write_pos - net->buff)));
    net->write_pos= net->buff;
  }
  /* Sync packet number if using compression */
  if (net->compress)
    net->pkt_nr=net->compress_pkt_nr;
  DBUG_RETURN(error);
}


/*****************************************************************************
** Write something to server/client buffer
*****************************************************************************/

/**
  Write a logical packet with packet header.

  Format: Packet length (3 bytes), packet number (1 byte)
  When compression is used, a 3 byte compression length is added.

  @note If compression is used, the original packet is modified!
*/

my_bool my_net_write(NET *net, const uchar *packet, size_t len)
{
  uchar buff[NET_HEADER_SIZE];

  if (unlikely(!net->vio)) /* nowhere to write */
    return 0;

  MYSQL_NET_WRITE_START(len);

  /*
    Big packets are handled by splitting them in packets of MAX_PACKET_LENGTH
    length. The last packet is always a packet that is < MAX_PACKET_LENGTH.
    (The last packet may even have a length of 0)
  */
  while (len >= MAX_PACKET_LENGTH)
  {
    const ulong z_size = MAX_PACKET_LENGTH;
    int3store(buff, z_size);
    buff[3]= (uchar) net->pkt_nr++;
    if (net_write_buff(net, buff, NET_HEADER_SIZE) ||
	net_write_buff(net, packet, z_size))
    {
      MYSQL_NET_WRITE_DONE(1);
      return 1;
    }
    packet += z_size;
    len-=     z_size;
  }
  /* Write last packet */
  int3store(buff,len);
  buff[3]= (uchar) net->pkt_nr++;
  if (net_write_buff(net, buff, NET_HEADER_SIZE))
  {
    MYSQL_NET_WRITE_DONE(1);
    return 1;
  }
#ifndef DEBUG_DATA_PACKETS
  DBUG_DUMP("packet_header", buff, NET_HEADER_SIZE);
#endif
  my_bool rc= MY_TEST(net_write_buff(net, packet, len));
  MYSQL_NET_WRITE_DONE(rc);
  return rc;
}


/**
  Send a command to the server.

    The reason for having both header and packet is so that libmysql
    can easy add a header to a special command (like prepared statements)
    without having to re-alloc the string.

    As the command is part of the first data packet, we have to do some data
    juggling to put the command in there, without having to create a new
    packet.
  
    This function will split big packets into sub-packets if needed.
    (Each sub packet can only be 2^24 bytes)

  @param net		NET handler
  @param command	Command in MySQL server (enum enum_server_command)
  @param header	Header to write after command
  @param head_len	Length of header
  @param packet	Query or parameter to query
  @param len		Length of packet

  @retval
    0	ok
  @retval
    1	error
*/

my_bool
net_write_command(NET *net,uchar command,
		  const uchar *header, size_t head_len,
		  const uchar *packet, size_t len)
{
  size_t length=len+1+head_len;			/* 1 extra byte for command */
  uchar buff[NET_HEADER_SIZE+1];
  uint header_size=NET_HEADER_SIZE+1;
  my_bool rc;
  DBUG_ENTER("net_write_command");
  DBUG_PRINT("enter",("length: %lu", (ulong) len));

#ifdef ENABLED_DEBUG_SYNC
  DBUG_EXECUTE_IF("simulate_error_on_packet_write",
                  {
                    if (command == COM_BINLOG_DUMP)
                    {
                      net->last_errno = ER_NET_ERROR_ON_WRITE;
                      DBUG_ASSERT(!debug_sync_set_action(
                      (THD *)net->thd,
                      STRING_WITH_LEN("now SIGNAL parked WAIT_FOR continue")));
                      DBUG_RETURN(true);
                    }
                  };);
#endif
  MYSQL_NET_WRITE_START(length);

  buff[4]=command;				/* For first packet */

  if (length >= MAX_PACKET_LENGTH)
  {
    /* Take into account that we have the command in the first header */
    len= MAX_PACKET_LENGTH - 1 - head_len;
    do
    {
      int3store(buff, MAX_PACKET_LENGTH);
      buff[3]= (uchar) net->pkt_nr++;
      if (net_write_buff(net, buff, header_size) ||
	  net_write_buff(net, header, head_len) ||
	  net_write_buff(net, packet, len))
      {
        MYSQL_NET_WRITE_DONE(1);
	DBUG_RETURN(1);
      }
      packet+= len;
      length-= MAX_PACKET_LENGTH;
      len= MAX_PACKET_LENGTH;
      head_len= 0;
      header_size= NET_HEADER_SIZE;
    } while (length >= MAX_PACKET_LENGTH);
    len=length;					/* Data left to be written */
  }
  int3store(buff,length);
  buff[3]= (uchar) net->pkt_nr++;
  rc= MY_TEST(net_write_buff(net, buff, header_size) ||
              (head_len && net_write_buff(net, header, head_len)) ||
              net_write_buff(net, packet, len) || net_flush(net));
  MYSQL_NET_WRITE_DONE(rc);
  DBUG_RETURN(rc);
}

/**
  Caching the data in a local buffer before sending it.

   Fill up net->buffer and send it to the client when full.

    If the rest of the to-be-sent-packet is bigger than buffer,
    send it in one big block (to avoid copying to internal buffer).
    If not, copy the rest of the data to the buffer and return without
    sending data.

  @param net		Network handler
  @param packet	Packet to send
  @param len		Length of packet

  @note
    The cached buffer can be sent as it is with 'net_flush()'.
    In this code we have to be careful to not send a packet longer than
    MAX_PACKET_LENGTH to net_real_write() if we are using the compressed
    protocol as we store the length of the compressed packet in 3 bytes.

  @retval
    0	ok
  @retval
    1
*/

static my_bool
net_write_buff(NET *net, const uchar *packet, size_t len)
{
  size_t left_length;
  if (net->compress && net->max_packet > MAX_PACKET_LENGTH)
    left_length= (MAX_PACKET_LENGTH - (net->write_pos - net->buff));
  else
    left_length= (net->buff_end - net->write_pos);

#ifdef DEBUG_DATA_PACKETS
  DBUG_DUMP("data_written", packet, len);
#endif
  if (len > left_length)
  {
    if (net->write_pos != net->buff)
    {
      /* Fill up already used packet and write it */
      memcpy((char*) net->write_pos,packet,left_length);
      if (net_real_write(net, net->buff, 
			 (size_t) (net->write_pos - net->buff) + left_length))
	return 1;
      net->write_pos= net->buff;
      packet+= left_length;
      len-= left_length;
    }
    if (net->compress)
    {
      /*
	We can't have bigger packets than 16M with compression
	Because the uncompressed length is stored in 3 bytes
      */
      left_length= MAX_PACKET_LENGTH;
      while (len > left_length)
      {
	if (net_real_write(net, packet, left_length))
	  return 1;
	packet+= left_length;
	len-= left_length;
      }
    }
    if (len > net->max_packet)
      return net_real_write(net, packet, len) ? 1 : 0;
    /* Send out rest of the blocks as full sized blocks */
  }
  if (len)
    memcpy((char*) net->write_pos,packet,len);
  net->write_pos+= len;
  return 0;
}


/**
  Read and write one packet using timeouts.
  If needed, the packet is compressed before sending.

  @todo
    - TODO is it needed to set this variable if we have no socket
*/

int
net_real_write(NET *net,const uchar *packet, size_t len)
{
  size_t length;
  const uchar *pos,*end;
  uint retry_count=0;
  DBUG_ENTER("net_real_write");

#if defined(MYSQL_SERVER)
  THD *thd= (THD *)net->thd;
#if defined(USE_QUERY_CACHE)
  query_cache_insert(thd, (char*) packet, len, net->pkt_nr);
#endif
  if (likely(thd))
  {
    /*
      Wait until pending operations (currently it is engine
      asynchronous group commit) are finished before replying
      to the client, to keep durability promise.
    */
    thd->async_state.wait_for_pending_ops();
  }
#endif

  if (unlikely(net->error == 2))
    DBUG_RETURN(-1);				/* socket can't be used */

  net->reading_or_writing=2;
#ifdef HAVE_COMPRESS
  if (net->compress)
  {
    size_t complen;
    uchar *b;
    uint header_length=NET_HEADER_SIZE+COMP_HEADER_SIZE;
    if (!(b= (uchar*) my_malloc(key_memory_NET_compress_packet,
                                len + NET_HEADER_SIZE + COMP_HEADER_SIZE + 1,
                                MYF(MY_WME | (net->thread_specific_malloc
                                              ? MY_THREAD_SPECIFIC : 0)))))
    {
      net->error= 2;
      net->last_errno= ER_OUT_OF_RESOURCES;
      /* In the server, the error is reported by MY_WME flag. */
      net->reading_or_writing= 0;
      DBUG_RETURN(1);
    }
    memcpy(b+header_length,packet,len);

    /* Don't compress error packets (compress == 2) */
    if (net->compress == 2 || my_compress(b+header_length, &len, &complen))
      complen=0;
    int3store(&b[NET_HEADER_SIZE],complen);
    int3store(b,len);
    b[3]=(uchar) (net->compress_pkt_nr++);
    len+= header_length;
    packet= b;
  }
#endif /* HAVE_COMPRESS */

#ifdef DEBUG_DATA_PACKETS
  DBUG_DUMP("data_written", packet, len);
#endif
  pos= packet;
  end=pos+len;
  while (pos != end)
  {
    length= vio_write(net->vio, pos, (size_t) (end - pos));
    if (ssize_t(length) <= 0)
    {
      bool interrupted= vio_should_retry(net->vio);
      if (interrupted || !length)
      {
        if (retry_count++ < net->retry_count)
          continue;
      }
      EXTRA_DEBUG_fprintf(stderr,
                          "%s: write looped on vio with state %d, aborting thread\n",
                          my_progname, (int) net->vio->type);
      net->error= 2;				/* Close socket */

      if (net->vio->state != VIO_STATE_SHUTDOWN || net->last_errno == 0)
      {
        net->last_errno= (interrupted ? ER_NET_WRITE_INTERRUPTED :
                          ER_NET_ERROR_ON_WRITE);
#ifdef MYSQL_SERVER
        if (global_system_variables.log_warnings > 3)
        {
          sql_print_warning("Could not write packet: fd: %lld  state: %d  "
                            "errno: %d  vio_errno: %d  length: %ld",
                            (longlong) vio_fd(net->vio), (int) net->vio->state,
                            vio_errno(net->vio), net->last_errno,
                            (ulong) (end-pos));
        }
#endif
      }
      MYSQL_SERVER_my_error(net->last_errno, MYF(0));
      break;
    }
    pos+=length;
    update_statistics(thd_increment_bytes_sent(net->thd, length));
  }
#ifdef HAVE_COMPRESS
  if (net->compress)
    my_free((void*) packet);
#endif
  net->reading_or_writing= 0;
  DBUG_RETURN(pos != end);
}

/**
  Try to parse and process proxy protocol header.

  This function is called in case MySQL packet header cannot be parsed.
  It checks if proxy header was sent, and that it was send from allowed remote
  host, as defined by proxy-protocol-networks parameter.

  If proxy header is parsed, then THD and ACL structures and changed to indicate
  the new peer address and port.

  Note, that proxy header can only be sent either when the connection is established,
  or as the client reply packet to
*/
#undef IGNORE                   /* for Windows */
typedef enum { RETRY, ABORT, IGNORE} handle_proxy_header_result;
static handle_proxy_header_result handle_proxy_header(NET *net)
{
#if !defined(MYSQL_SERVER) || defined(EMBEDDED_LIBRARY)
  return IGNORE;
#else
  THD *thd= (THD *)net->thd;

  if (!has_proxy_protocol_header(net) || !thd ||
      thd->get_command() != COM_CONNECT)
    return IGNORE;

  /*
    Proxy information found in the first 4 bytes received so far.
    Read and parse proxy header , change peer ip address and port in THD.
  */
  proxy_peer_info peer_info;

  if (!thd->net.vio)
  {
    DBUG_ASSERT_NO_ASSUME(0);
    return ABORT;
  }

  if (!is_proxy_protocol_allowed((sockaddr *)&(thd->net.vio->remote)))
  {
     /* proxy-protocol-networks variable needs to be set to allow this remote address */
     my_printf_error(ER_HOST_NOT_PRIVILEGED, "Proxy header is not accepted from %s",
       MYF(0), thd->main_security_ctx.ip);
     return ABORT;
  }

  if (parse_proxy_protocol_header(net, &peer_info))
  {
     /* Failed to parse proxy header*/
     my_printf_error(ER_UNKNOWN_ERROR, "Failed to parse proxy header", MYF(0));
     return ABORT;
  }

  if (peer_info.is_local_command)
    /* proxy header indicates LOCAL connection, no action necessary */
    return RETRY;
  /* Change peer address in THD and ACL structures.*/
  uint host_errors;
  net->using_proxy_protocol= 1;
  return (handle_proxy_header_result)thd_set_peer_addr(thd,
                         &(peer_info.peer_addr), NULL, peer_info.port,
                         false, &host_errors);
#endif
}

/**
  Reads one packet to net->buff + net->where_b.
  Long packets are handled by my_net_read().
  This function reallocates the net->buff buffer if necessary.

  @return
    Returns length of packet.
*/

static ulong my_real_read(NET *net, size_t *complen,
                          my_bool header __attribute__((unused)))
{
  uchar *pos;
  size_t length;
  uint i,retry_count=0;
  ulong len=packet_error;
  my_bool expect_error_packet __attribute__((unused))= 0;
retry:

  uint32 remain= (net->compress ? NET_HEADER_SIZE+COMP_HEADER_SIZE :
		  NET_HEADER_SIZE);
  size_t count= remain;
  struct st_net_server *server_extension= 0;

  if (header)
  {
    server_extension= static_cast<st_net_server*> (net->extension);
    if (server_extension != NULL)
    {
      void *user_data= server_extension->m_user_data;
      server_extension->m_before_header(net, user_data, count);
    }
  }

  *complen = 0;

  net->reading_or_writing=1;
  pos = net->buff + net->where_b;
  for (i=0; i < 2 ; i++)
  {
    while (remain > 0)
    {
      length= vio_read(net->vio, pos, remain);
      if ((ssize_t) length <= 0)
      {
        DBUG_PRINT("info", ("vio_read returned %lld  errno: %d",
                            (long long) length, vio_errno(net->vio)));
        if (i == 0 && unlikely(thd_net_is_killed((THD *) net->thd)))
        {
          DBUG_PRINT("info", ("thd is killed"));
          len= packet_error;
          net->error= 0;
          net->last_errno= ER_CONNECTION_KILLED;
          MYSQL_SERVER_my_error(net->last_errno, MYF(0));
          goto end;
        }
        if (vio_should_retry(net->vio) && retry_count++ < net->retry_count)
          continue;
        EXTRA_DEBUG_fprintf(stderr,
                            "%s: read looped with error %d on vio with state %d, "
                            "aborting thread\n",
                            my_progname, vio_errno(net->vio), (int) net->vio->type);
        DBUG_PRINT("error",
                   ("Couldn't read packet: remain: %u  errno: %d  length: %ld",
                    remain, vio_errno(net->vio), (long) length));
        len= packet_error;
        net->error= 2;
        net->last_errno= (vio_was_timeout(net->vio) ? ER_NET_READ_INTERRUPTED
                                                    : ER_NET_READ_ERROR);
#ifdef MYSQL_SERVER
          strmake_buf(net->last_error, ER(net->last_errno));
          if (global_system_variables.log_warnings > 3)
          {
            /* Log things as a warning */
            sql_print_warning("Could not read packet: fd: %lld  state: %d  "
                              "read_length: %u  errno: %d  vio_errno: %d  "
                              "length: %lld",
                              (longlong) vio_fd(net->vio),
                              (int) net->vio->state,
                              remain, vio_errno(net->vio), net->last_errno,
                              (longlong) length);
          }
          my_error(net->last_errno, MYF(0));
#endif /* MYSQL_SERVER */
        goto end;
      }
      remain-= (uint32) length;
      pos+= length;
      update_statistics(thd_increment_bytes_received(net->thd, length));
    }
#ifdef DEBUG_DATA_PACKETS
    DBUG_DUMP("data_read", net->buff + net->where_b, length);
#endif
    if (i == 0)
    {
      /* First part is packet length */
      size_t helping;
#ifndef DEBUG_DATA_PACKETS
      DBUG_DUMP("packet_header", net->buff + net->where_b, NET_HEADER_SIZE);
#endif
      if (net->buff[net->where_b + 3] != (uchar) net->pkt_nr)
      {
        if (net->pkt_nr_can_be_reset)
        {
          /*
            We are using a protocol like semi-sync where master and slave
            sends packets in parallel.
            Copy current one as it can be useful for debugging.
          */
          net->pkt_nr= net->buff[net->where_b + 3];
        }
        else
        {
#ifndef MYSQL_SERVER
          if (net->buff[net->where_b + 3] == (uchar) (net->pkt_nr -1))
          {
            /*
              If the server was killed then the server may have missed the
              last sent client packet and the packet numbering may be one off.
            */
            DBUG_PRINT("warning", ("Found possible out of order packets"));
            expect_error_packet= 1;
          }
          else
#endif
            goto packets_out_of_order;
        }
      }
      net->compress_pkt_nr= ++net->pkt_nr;
#ifdef HAVE_COMPRESS
      if (net->compress)
      {
        /*
         The following uint3korr() may read 4 bytes, so make sure we don't
         read unallocated or uninitialized memory. The right-hand expression
         must match the size of the buffer allocated in net_realloc().
        */
        DBUG_ASSERT(net->where_b + NET_HEADER_SIZE + sizeof(uint32) <=
                    net->max_packet + NET_HEADER_SIZE + COMP_HEADER_SIZE + 1);
        /*
          If the packet is compressed then complen > 0 and contains the
          number of bytes in the uncompressed packet
        */
        *complen= uint3korr(&(net->buff[net->where_b + NET_HEADER_SIZE]));
      }
#endif

      len= uint3korr(net->buff + net->where_b);
      if (!len) /* End of big multi-packet */
        goto end;
      helping= MY_MAX(len, *complen) + net->where_b;
      /* The necessary size of net->buff */
      if (helping >= net->max_packet)
      {
        if (net_realloc(net, helping))
        {
          len= packet_error; /* Return error and close connection */
          goto end;
        }
      }
      pos= net->buff + net->where_b;
      remain= (uint32) len;
      if (server_extension != NULL)
      {
        void *user_data= server_extension->m_user_data;
        server_extension->m_after_header(net, user_data, count, 0);
        server_extension= NULL;
      }
    }
#ifndef MYSQL_SERVER
    else if (expect_error_packet)
    {
      /*
        This check is safe both for compressed and not compressed protocol
        as for the compressed protocol errors are not compressed anymore.
      */
      if (net->buff[net->where_b] != (uchar) 255)
      {
        /* Restore pkt_nr to original value */
        net->pkt_nr--;
        goto packets_out_of_order;
      }
    }
#endif
  }

end:
  net->reading_or_writing=0;
#ifdef DEBUG_DATA_PACKETS
  if (len != packet_error)
    DBUG_DUMP("data_read", net->buff+net->where_b, len);
#endif
  if (server_extension != NULL)
  {
    void *user_data= server_extension->m_user_data;
    server_extension->m_after_header(net, user_data, count, 1);
    DBUG_ASSERT(len == packet_error || len == 0);
  }
  return(len);

packets_out_of_order :
  switch (handle_proxy_header(net))
  {
  case ABORT:
    /* error happened, message is already written. */
    len= packet_error;
    goto end;
  case RETRY:
    goto retry;
  case IGNORE:
    break;
  }
  DBUG_PRINT("error", ("Packets out of order (Found: %d, expected %u)",
                       (int) net->buff[net->where_b + 3], net->pkt_nr));
  EXTRA_DEBUG_ASSERT(0);
  /*
     We don't make noise server side, since the client is expected
     to break the protocol for e.g. --send LOAD DATA .. LOCAL where
     the server expects the client to send a file, but the client
     may reply with a new command instead.
  */
  len= packet_error;
  MYSQL_SERVER_my_error(ER_NET_PACKETS_OUT_OF_ORDER, MYF(0));
  goto end;
}



/* Old interface. See my_net_read_packet() for function description */

#undef my_net_read

ulong my_net_read(NET *net)
{
  return my_net_read_packet(net, 0);
}


/**
  Read a packet from the client/server and return it without the internal
  package header.

  If the packet is the first packet of a multi-packet packet
  (which is indicated by the length of the packet = 0xffffff) then
  all sub packets are read and concatenated.

  If the packet was compressed, its uncompressed and the length of the
  uncompressed packet is returned.

  read_from_server is set when the server is reading a new command
  from the client.

  @return
  The function returns the length of the found packet or packet_error.
  net->read_pos points to the read data.
*/
ulong
my_net_read_packet(NET *net, my_bool read_from_server)
{
  ulong reallen = 0;
  ulong length;
  DBUG_ENTER("my_net_read_packet");
  length= my_net_read_packet_reallen(net, read_from_server, &reallen);
  DBUG_RETURN(length);
}


ulong
my_net_read_packet_reallen(NET *net, my_bool read_from_server, ulong* reallen)
{
  size_t len, complen;

  MYSQL_NET_READ_START();

  *reallen = 0;
#ifdef HAVE_COMPRESS
  if (!net->compress)
  {
#endif
    len = my_real_read(net,&complen, read_from_server);
    if (len == MAX_PACKET_LENGTH)
    {
      /* First packet of a multi-packet.  Concatenate the packets */
      ulong save_pos = net->where_b;
      size_t total_length= 0;
      do
      {
	net->where_b += (ulong)len;
	total_length += len;
	len = my_real_read(net,&complen, 0);
      } while (len == MAX_PACKET_LENGTH);
      if (likely(len != packet_error))
	len+= total_length;
      net->where_b = save_pos;
    }

    net->read_pos = net->buff + net->where_b;
    if (likely(len != packet_error))
    {
      net->read_pos[len]=0;		/* Safeguard for mysql_use_result */
      *reallen = (ulong)len;
    }
    MYSQL_NET_READ_DONE(0, len);
    return (ulong)len;
#ifdef HAVE_COMPRESS
  }
  else
  {
    /* We are using the compressed protocol */

    ulong buf_length;
    ulong start_of_packet;
    ulong first_packet_offset;
    uint read_length, multi_byte_packet=0;

    if (net->remain_in_buf)
    {
      buf_length= net->buf_length;		/* Data left in old packet */
      first_packet_offset= start_of_packet= (net->buf_length -
					     net->remain_in_buf);
      /* Restore the character that was overwritten by the end 0 */
      net->buff[start_of_packet]= net->save_char;
    }
    else
    {
      /* reuse buffer, as there is nothing in it that we need */
      buf_length= start_of_packet= first_packet_offset= 0;
    }
    for (;;)
    {
      ulong packet_len;

      if (buf_length - start_of_packet >= NET_HEADER_SIZE)
      {
	read_length = uint3korr(net->buff+start_of_packet);
	if (!read_length)
	{ 
	  /* End of multi-byte packet */
	  start_of_packet += NET_HEADER_SIZE;
	  break;
	}
	if (read_length + NET_HEADER_SIZE <= buf_length - start_of_packet)
	{
	  if (multi_byte_packet)
	  {
	    /* Remove packet header for second packet */
	    memmove(net->buff + first_packet_offset + start_of_packet,
		    net->buff + first_packet_offset + start_of_packet +
		    NET_HEADER_SIZE,
		    buf_length - start_of_packet);
	    start_of_packet += read_length;
	    buf_length -= NET_HEADER_SIZE;
	  }
	  else
	    start_of_packet+= read_length + NET_HEADER_SIZE;

	  if (read_length != MAX_PACKET_LENGTH)	/* last package */
	  {
	    multi_byte_packet= 0;		/* No last zero len packet */
	    break;
	  }
	  multi_byte_packet= NET_HEADER_SIZE;
	  /* Move data down to read next data packet after current one */
	  if (first_packet_offset)
	  {
	    memmove(net->buff,net->buff+first_packet_offset,
		    buf_length-first_packet_offset);
	    buf_length-=first_packet_offset;
	    start_of_packet -= first_packet_offset;
	    first_packet_offset=0;
	  }
	  continue;
	}
      }
      /* Move data down to read next data packet after current one */
      if (first_packet_offset)
      {
	memmove(net->buff,net->buff+first_packet_offset,
		buf_length-first_packet_offset);
	buf_length-=first_packet_offset;
	start_of_packet -= first_packet_offset;
	first_packet_offset=0;
      }

      net->where_b=buf_length;
      if ((packet_len = my_real_read(net,&complen, read_from_server))
          == packet_error)
      {
        MYSQL_NET_READ_DONE(1, 0);
	return packet_error;
      }
      read_from_server= 0;
      if (my_uncompress(net->buff + net->where_b, packet_len,
			&complen))
      {
	net->error= 2;			/* caller will close socket */
        net->last_errno= ER_NET_UNCOMPRESS_ERROR;
	MYSQL_SERVER_my_error(ER_NET_UNCOMPRESS_ERROR, MYF(0));
        MYSQL_NET_READ_DONE(1, 0);
	return packet_error;
      }
      buf_length+= (ulong)complen;
      *reallen += packet_len;
    }

    net->read_pos=      net->buff+ first_packet_offset + NET_HEADER_SIZE;
    net->buf_length=    buf_length;
    net->remain_in_buf= (ulong) (buf_length - start_of_packet);
    len = ((ulong) (start_of_packet - first_packet_offset) - NET_HEADER_SIZE -
           multi_byte_packet);
    net->save_char= net->read_pos[len];	/* Must be saved */
    net->read_pos[len]=0;		/* Safeguard for mysql_use_result */
  }
#endif /* HAVE_COMPRESS */
  MYSQL_NET_READ_DONE(0, len);
  return (ulong)len;
}


void my_net_set_read_timeout(NET *net, uint timeout)
{
  DBUG_ENTER("my_net_set_read_timeout");
  DBUG_PRINT("enter", ("timeout: %d", timeout));
  if (net->read_timeout != timeout)
  {
    net->read_timeout= timeout;
    if (net->vio)
      vio_timeout(net->vio, 0, timeout);
  }
  DBUG_VOID_RETURN;
}


void my_net_set_write_timeout(NET *net, uint timeout)
{
  DBUG_ENTER("my_net_set_write_timeout");
  DBUG_PRINT("enter", ("timeout: %d", timeout));
  if (net->write_timeout != timeout)
  {
    net->write_timeout= timeout;
    if (net->vio)
      vio_timeout(net->vio, 1, timeout);
  }
  DBUG_VOID_RETURN;
}
