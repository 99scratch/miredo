/*
 * teredo-udp.cpp - UDP sockets class definition
 * $Id: teredo-udp.cpp,v 1.3 2004/08/22 15:38:26 rdenisc Exp $
 *
 * See "Teredo: Tunneling IPv6 over UDP through NATs"
 * for more information
 */

/***********************************************************************
 *  Copyright (C) 2004 Remi Denis-Courmont.                            *
 *  This program is free software; you can redistribute and/or modify  *
 *  it under the terms of the GNU General Public License as published  *
 *  by the Free Software Foundation; version 2 of the license.         *
 *                                                                     *
 *  This program is distributed in the hope that it will be useful,    *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of     *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.               *
 *  See the GNU General Public License for more details.               *
 *                                                                     *
 *  You should have received a copy of the GNU General Public License  *
 *  along with this program; if not, you can get it from:              *
 *  http://www.gnu.org/copyleft/gpl.html                               *
 ***********************************************************************/

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <inttypes.h>
#include <string.h> // memset()

#include <v4global.h> // is_ipv4_global_unicast()
#include "teredo-udp.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h> // close()
#include <sys/select.h>
#include <netinet/in.h>

#include <syslog.h> // syslog()


/*
 * Opens a Teredo UDP/IPv4 socket.
 */
static int
OpenTeredoSocket (uint32_t bind_ip, uint16_t port)
{
	struct sockaddr_in myaddr;
	memset (&myaddr, 0, sizeof (myaddr));
	myaddr.sin_family = AF_INET;
	myaddr.sin_port = port;
	myaddr.sin_addr.s_addr = bind_ip;
#ifdef HAVE_SA_LEN
	myaddr.sin_len = sizeof (myaddr);
#endif

	int fd = socket (PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd == -1)
	{
		syslog (LOG_ALERT, _("Fatal socket error: %m\n"));
		return -1; // failure
	}

	if (bind (fd, (struct sockaddr *)&myaddr, sizeof (myaddr)))
	{
		syslog (LOG_ALERT, _("Fatal bind error: %m\n"));
		return -1;
	}

	int t = 1;
	setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &t, sizeof (t));
#ifdef IP_PMTUDISC_DONT
	/* 
	 * This tells the (Linux) kernel not to set the Don't Fragment flags
	 * on UDP packets we send. This is recommended by the Teredo
	 * specifiation.
	 */
	t = IP_PMTUDISC_DONT;
	setsockopt (fd, SOL_IP, IP_MTU_DISCOVER, &t, sizeof (t));
#endif
	return fd;
}


static int
SendUDPPacket (int fd, const void *packet, size_t plen,
		uint32_t dest_ip, uint16_t dest_port)
{
	struct sockaddr_in nat_addr;
	memset (&nat_addr, 0, sizeof (nat_addr));
	nat_addr.sin_family = AF_INET;
	nat_addr.sin_port = dest_port;
	nat_addr.sin_addr.s_addr = dest_ip;
#ifdef HAVE_SA_LEN
	nat_addr.sin_len = sizeof (nat_addr);
#endif

	int check = sendto (fd, packet, plen, 0, (struct sockaddr *)&nat_addr,
				sizeof (nat_addr));
	if (check == -1)
	{
		syslog (LOG_WARNING, _("Couldn't send UDP packet: %m\n"));
		return -1;
	}
	else if ((size_t)check < plen)
	{
		syslog (LOG_WARNING, _("UDP packet shortened: sent %d bytes "
				"instead of %u\n"), check, plen);
		return -1;
	}
	return 0;
}


/*** TeredoCommonUDP implementation ***/
TeredoCommonUDP::~TeredoCommonUDP (void)
{
}


/*
 * Parses a Teredo packet header. Blocking function.
 * FIXME: re-entrancy
 */
int
TeredoCommonUDP::ReceivePacket (int fd)
{
	uint8_t buffer[65507];
	int length;

	// Receive a UDP packet
	{
		struct sockaddr_in ad;
		socklen_t alen = sizeof (ad);

		length = recvfrom (fd, buffer, sizeof (buffer), 0,
					(struct sockaddr *)&ad, &alen);

		if (length < 0)
		{
			syslog (LOG_WARNING,
				_("Error receiving UDP packet: %m\n"));
			return -1;
		}

		last_ip = ad.sin_addr.s_addr;
		last_port = ad.sin_port;
	}

	// Check type of Teredo header:
	uint8_t *ptr = buffer;
	orig = NULL;
	nonce = NULL;

	// Parse Teredo headers
	while ((length >= 2) && ptr[0] == 0)
	{
		switch (ptr[1])
		{
			// Teredo Authentication header
			case teredo_auth_hdr:
			{
				ptr += 2;
				/* ID and Auth */
				length -= 4;
				if (length < 0)
					return -1; // too small

				uint8_t id_len = *ptr;
				ptr++;
				uint8_t au_len = *ptr;
				ptr++;

				length -= id_len + au_len;
				/* TODO: secure qualification */
				ptr += id_len + au_len;

				/* Nonce */
				length -= sizeof (nonce_buf);
				if (length < 0)
					return -1;

				memcpy (nonce_buf, ptr, sizeof (nonce_buf));
				nonce = nonce_buf;
				ptr += sizeof (nonce_buf);

				/* Confirmation */
				length --;
				if (length < 0)
					return -1;

				if (/* confirmation = */ *(ptr++))
					// confirmation byte MUST be 0
					return -1;
				break;
			}

			// Teredo Origin Indication
			case teredo_orig_ind:
			{
				length -= sizeof (orig_buf);
				if (length < 0)
					return -1; // too small

				memcpy (&orig_buf, ptr, sizeof (orig_buf));
				orig = &orig_buf;
				ptr += sizeof (orig_buf);
				break;
			}

			// Unknown Teredo header
			default:
				return -1; // malformatted packet
		}
	}

	if (length < 0)
		return -1;

	// length <= 65507
	memcpy (&ipv6_buf.ip6, ptr, length);
	ip6len = length;

	return 0;
}



/*** TeredoRelayUDP implementation ***/
TeredoRelayUDP::~TeredoRelayUDP (void)
{
	if (fd != -1)
		close (fd);
}


int TeredoRelayUDP::ListenPort (uint16_t port)
{
	// Closes former socket:
	if (fd != -1)
		close (fd);

	fd = OpenTeredoSocket (INADDR_ANY, port);
	return fd != -1 ? 0 : -1;
}


int
TeredoRelayUDP::RegisterReadSet (fd_set *readset) const
{
	if (fd != -1)
		FD_SET (fd, readset);
	return fd;
}


int
TeredoRelayUDP::ReceivePacket (void)
{
	if (fd != -1)
	{
		fd_set set;

		FD_ZERO (&set);
		FD_SET (fd, &set);

		struct timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 0;

		// FIXME:
		// decide whether we should block or not
		// (at the moment: non-blocking but documented as
		// blocking!!)
		// and make this thread-blocking-safe.
		if (select (fd + 1, &set, NULL, NULL, &tv) == 1)
			return TeredoCommonUDP::ReceivePacket (fd);
	}
	return -1;
}


int
TeredoRelayUDP::SendPacket (const void *packet, size_t len,
				uint32_t dest_ip, uint16_t dest_port) const
{
	return SendUDPPacket (fd, packet, len, dest_ip, dest_port);
}


/*** TeredoServerUDP implementation ***/

TeredoServerUDP::~TeredoServerUDP ()
{
	if (fd_primary != -1)
		close (fd_primary);
	if (fd_secondary != -1)
		close (fd_secondary);
}


int TeredoServerUDP::ListenIP (uint32_t ip1, uint32_t ip2)
{
	if (!is_ipv4_global_unicast (ip1)
	 || !is_ipv4_global_unicast (ip2))
	{
		syslog (LOG_ERR, _("Teredo server UDP socket error: "
			"Server IPv4 addresses must be global unicast."));
		return -1;
	}

	if (ip1 == INADDR_ANY || ip2 == INADDR_ANY)
	{
		syslog (LOG_ERR, _("Teredo server UDP socket error: "
			"Server IPv4 addresses must not be wildcard.\n"));
		return -1;
	}

	if (fd_primary != -1)
		close (fd_primary);
	fd_primary = OpenTeredoSocket (ip1, htons (IPPORT_TEREDO));
	if (fd_primary == -1)
		return -1;

	if (fd_secondary != -1)
		close (fd_secondary);
	fd_secondary = OpenTeredoSocket (ip2, htons (IPPORT_TEREDO));
	if (fd_secondary == -1)
		return -1;

	return 0;
}


int TeredoServerUDP::RegisterReadSet (fd_set *readset) const
{
	int maxfd = -1;
	if (fd_primary != -1)
	{
		FD_SET (fd_primary, readset);
		if (fd_primary > maxfd)
			maxfd = fd_primary;
	}
	
	if (fd_secondary != -1)
	{
		FD_SET (fd_secondary, readset);
		if (fd_secondary > maxfd)
			maxfd = fd_secondary;
	}
	return maxfd;
}


int
TeredoServerUDP::ReceivePacket (void)
{
	/* Is there a packet on any of the UDP sockets? */
	fd_set set;
	int fd = RegisterReadSet (&set);

	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	
	if (select (fd, &set, NULL, NULL, &tv) <= 0)
		return -1;

	if ((fd_primary != -1) && FD_ISSET (fd_primary, &set))
	{
		fd = fd_primary;
		was_secondary = false;
	}
	else
	if ((fd_secondary != -1) && FD_ISSET (fd_secondary, &set))
	{
		fd = fd_secondary;
		was_secondary = true;
	}
	
	return (fd == -1) ? -1 : TeredoCommonUDP::ReceivePacket (fd);
}


int
TeredoServerUDP::SendPacket (const void *packet, size_t len,
				uint32_t dest_ip, uint16_t dest_port,
				bool use_secondary_ip) const
{
	return SendUDPPacket (use_secondary_ip ? fd_secondary : fd_primary,
				packet, len, dest_ip, dest_port);
}