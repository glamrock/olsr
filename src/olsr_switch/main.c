
/*
 * The olsr.org Optimized Link-State Routing daemon(olsrd)
 * Copyright (c) 2005, Andreas T�nnesen(andreto@olsr.org)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions 
 * are met:
 *
 * * Redistributions of source code must retain the above copyright 
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright 
 *   notice, this list of conditions and the following disclaimer in 
 *   the documentation and/or other materials provided with the 
 *   distribution.
 * * Neither the name of olsr.org, olsrd nor the names of its 
 *   contributors may be used to endorse or promote products derived 
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE 
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, 
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; 
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER 
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT 
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN 
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE 
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Visit http://www.olsr.org for more information.
 *
 * If you find this software useful feel free to make a donation
 * to the project. For more information see the website or contact
 * the copyright holders.
 *
 * $Id: main.c,v 1.7 2005/05/31 08:55:23 kattemat Exp $
 */

/* olsrd host-switch daemon */

#ifdef WIN32
#define close(x) closesocket(x)
int __stdcall SignalHandler(unsigned long signal);
#else
void
olsr_shutdown(int);
#endif

#include "olsr_host_switch.h"
#include "link_rules.h"
#include "ohs_cmd.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static int srv_socket;

#define OHS_BUFSIZE 1500
static olsr_u8_t data_buffer[OHS_BUFSIZE];

struct ohs_connection *ohs_conns;


static int ip_version;
int ipsize;
static char ipv6_buf[100]; /* for address coversion */

olsr_u32_t logbits;

char *
olsr_ip_to_string(union olsr_ip_addr *addr)
{
  static int index = 0;
  static char buff[4][100];
  char *ret;
  struct in_addr in;
  
  if(ip_version == AF_INET)
    {
      in.s_addr=addr->v4;
      ret = inet_ntoa(in);
    }
  else
    {
      /* IPv6 */
      ret = (char *)inet_ntop(AF_INET6, &addr->v6, ipv6_buf, sizeof(ipv6_buf));
    }

  strncpy(buff[index], ret, 100);

  ret = buff[index];

  index = (index + 1) & 3;

  return ret;
}


#ifdef WIN32
int __stdcall
SignalHandler(unsigned long signal)
#else
void
ohs_close(int signal)
#endif
{
  printf("OHS: exit\n");

  close(srv_socket);

  exit(0);
}

struct ohs_connection *
get_client_by_addr(union olsr_ip_addr *adr)
{
  struct ohs_connection *oc = ohs_conns;

  while(oc)
    {
      if(COMP_IP(adr, &oc->ip_addr))
        return oc;
      oc = oc->next;
    }
  return NULL;
}


int
ohs_init_new_connection(int s)
{
  struct ohs_connection *oc;
  olsr_u8_t new_addr[4];

  if(logbits & LOG_CONNECT)
    printf("ohs_init_new_connection\n");

  /* Create new client node */
  oc = malloc(sizeof(struct ohs_connection));
  if(!oc)
    OHS_OUT_OF_MEMORY("New connection");

  memset(oc, 0, sizeof(oc));

  oc->socket = s;

  oc->links = NULL;
  oc->rx = 0;
  oc->tx = 0;
  oc->linkcnt = 0;

  /* Queue */
  oc->next = ohs_conns;
  ohs_conns = oc;

  /* Get "fake IP" */
  if(recv(oc->socket, new_addr, 4, 0) != 4)
    {
      printf("Failed to fetch IP address!\n");
      return -1;
    }
  memcpy(&oc->ip_addr, new_addr, 4);
  oc->ip_addr.v4 = ntohl(oc->ip_addr.v4);
  if(logbits & LOG_CONNECT)
    printf("IP: %s\n", olsr_ip_to_string(&oc->ip_addr));

  return 1;
}

int
ohs_delete_connection(struct ohs_connection *oc)
{

  /* Close the socket */
  close(oc->socket);

  if(logbits & LOG_CONNECT)
    printf("Removing entry %s\n", olsr_ip_to_string(&oc->ip_addr));
  /* De-queue */
  if(oc == ohs_conns)
    {
      ohs_conns = ohs_conns->next;
    }
  else
    {
      struct ohs_connection *curr_entry, *prev_entry;
      curr_entry = ohs_conns->next;
      prev_entry = ohs_conns;
      
      while(curr_entry)
	{
	  if(curr_entry == oc)
	    {
	      prev_entry->next = curr_entry->next;
	      break;
	    }
	  prev_entry = curr_entry;
	  curr_entry = curr_entry->next;
	}
    }
  /* Free */
  free(oc);

  return 0;
}

int
ohs_route_data(struct ohs_connection *oc)
{
  struct ohs_connection *ohs_cs;
  ssize_t len;
  int cnt = 0;

  oc->tx++;
  /* Read data */
  if((len = recv(oc->socket, data_buffer, OHS_BUFSIZE, 0)) <= 0)
    return -1;

  if(logbits & LOG_FORWARD)
    printf("Received %d bytes from %s\n", len, olsr_ip_to_string(&oc->ip_addr));

  /* Loop trough clients */
  for(ohs_cs = ohs_conns; ohs_cs; ohs_cs = ohs_cs->next)
    {
      /* Check that the link is active open */
      if(ohs_check_link(oc, &ohs_cs->ip_addr) &&
	 oc->socket != ohs_cs->socket)
	{
	  ssize_t sent;

	  /* Send link addr */
	  if(send(ohs_cs->socket, oc->ip_addr.v6.s6_addr, ipsize, 0) != ipsize)
	    {
	      printf("Error sending link address!\n");
	    }
	  /* Send data */
	  if(logbits & LOG_FORWARD)
	    printf("Sending %d bytes %s=>%s\n", len, 
		   olsr_ip_to_string(&oc->ip_addr),
		   olsr_ip_to_string(&ohs_cs->ip_addr));

	  if((sent = send(ohs_cs->socket, data_buffer, len, 0)) != len)
	    {
	      printf("Error sending(buf %d != sent %d)\n", len, sent);
	    }
	  ohs_cs->rx++;
	  cnt++;
	}
    }

  return cnt;
}

int
ohs_init_connect_sockets()
{
  olsr_u32_t yes = 1;
  struct sockaddr_in sin;

  printf("Initiating socket TCP port %d\n", OHS_TCP_PORT);

  if((srv_socket = socket(PF_INET, SOCK_STREAM, 0)) < 0)
    {
      printf("Could not initialize socket: %s\n", strerror(errno));
      exit(0);
    }

  if(setsockopt(srv_socket, SOL_SOCKET, SO_REUSEADDR, 
		(char *)&yes, sizeof(yes)) < 0) 
    {
      printf("SO_REUSEADDR failed for socket: %s\n", strerror(errno));
      close(srv_socket);
      exit(0);
    }

  /* complete the socket structure */
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = INADDR_ANY;
  sin.sin_port = htons(OHS_TCP_PORT);
  
  /* bind the socket to the port number */
  if (bind(srv_socket, (struct sockaddr *) &sin, sizeof(sin)) == -1) 
    {
      printf("bind failed for socket: %s\n", strerror(errno));
      exit(0);
    }
  
  /* show that we are willing to listen */
  if (listen(srv_socket, 5) == -1) 
    {
      printf("listen failed for socket: %s\n", strerror(errno));
      exit(0);
    }
  return 1;
}


int
ohs_configure()
{

  return 1;
}

void
ohs_listen_loop()
{
  int n;
  fd_set ibits;
  int fn_stdin = fileno(stdin);


  printf("OHS command interper reading from STDIN\n");
  printf("\n> ");
  fflush(stdout);

  while(1)
    {
      int high;

      struct ohs_connection *ohs_cs;

      high = 0;
      FD_ZERO(&ibits);

      /* Add server socket */
      high = srv_socket;
      FD_SET(srv_socket, &ibits);


      if(fn_stdin > high) 
	high = fn_stdin;

      FD_SET(fn_stdin, &ibits);

      /* Add clients */
      for(ohs_cs = ohs_conns; ohs_cs; ohs_cs = ohs_cs->next)
	{
	  if(ohs_cs->socket > high)
	    high = ohs_cs->socket;
      
	  FD_SET(ohs_cs->socket, &ibits);
	}

      /* block */
      n = select(high + 1, &ibits, 0, 0, NULL);
      
      if(n == 0)
	continue;

      /* Did somethig go wrong? */
      if (n < 0) 
	{
	  if(errno == EINTR)
	    continue;
	  
	  printf("Error select: %s", strerror(errno));
	  continue;
	}
      
      /* Check server socket */
      if(FD_ISSET(srv_socket, &ibits))
	{
	  struct sockaddr_in pin;
	  socklen_t addrlen = sizeof(pin);
	  int s;
	  
	  memset(&pin, 0 , sizeof(pin));

	  if((s = accept(srv_socket, (struct sockaddr *)&pin, &addrlen)) < 0)
	    {
	      printf("accept failed socket: %s\n", strerror(errno));
	    }
	  else
	    {
	      /* Create new node */
	      ohs_init_new_connection(s);
	    }
	}
      /* Loop trough clients */
      ohs_cs = ohs_conns;
      while(ohs_cs)
	{
	  struct ohs_connection *ohs_tmp = ohs_cs;
	  ohs_cs = ohs_cs->next;

	  if(FD_ISSET(ohs_tmp->socket, &ibits))
	    {
	      if(ohs_route_data(ohs_tmp) < 0)
		  ohs_delete_connection(ohs_tmp);
	    }
	}

      if(FD_ISSET(fn_stdin, &ibits))
	{
	  ohs_parse_command(stdin);
	  printf("\n> ");
	  fflush(stdout);
	}      
    }
}


int
main(int argc, char *argv[])
{

  printf("olsrd host-switch daemon version %s starting\n", OHS_VERSION);

  logbits = LOG_DEFAULT;
  ip_version = AF_INET;
  ipsize = 4;

  srand((unsigned int)time(NULL));

  ohs_init_connect_sockets();
#ifdef WIN32
  SetConsoleCtrlHandler(SignalHandler, OLSR_TRUE);
#else
  signal(SIGINT, ohs_close);  
  signal(SIGTERM, ohs_close);  
#endif

  ohs_configure();

  ohs_listen_loop();

  ohs_close(0);

  return 1;
}