/*
 * conf.cpp - Configuration text file parsing definition
 * $Id: main.c 340 2004-12-31 13:28:42Z remi $
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

#include <gettext.h>

#include <stdio.h>
#include <stdlib.h> // malloc(), free()
#include <stdint.h>
#include <string.h>
#define stricmp( a, b ) strcasecmp( a, b )
#include <syslog.h>

#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <libteredo/teredo.h>

#include "conf.h"

MiredoConf::MiredoConf (void) : head (NULL)
{
}


MiredoConf::~MiredoConf (void)
{
	struct setting *ptr = head;

	while (ptr != NULL)
	{
		struct setting *buf = ptr->next;
		free (ptr->name);
		free (ptr->value);
		free (ptr);
		ptr = buf;
	}
}


bool
MiredoConf::Set (const char *name, const char *value, unsigned line)
{
	struct setting *parm =
		(struct setting *)malloc (sizeof (struct setting));

	if (parm != NULL)
	{
		parm->name = strdup (name);
		if (parm->name != NULL)
		{
			parm->value = strdup (value);
			if (parm->value != NULL)
			{
				parm->line = line;
				parm->next = NULL;

				/* lock here */
				if (head == NULL)
					head = parm;
				else
					tail->next = parm;
				tail = parm;
				/* unlock here */

				return true;
			}
			free (parm->name);
		}
		free (parm);
	}
	syslog (LOG_ALERT, _("Memory problem: %m"));

	return false;
}


bool
MiredoConf::ReadFile (FILE *stream)
{
	char lbuf[1056];
	unsigned line = 1;

	while (fgets (lbuf, sizeof (lbuf), stream) != NULL)
	{
		size_t len = strlen (lbuf) - 1;

		if (lbuf[len] != '\n')
		{
			while (fgetc (stream) != '\n')
				if (feof (stream) || ferror (stream))
					break;
			syslog (LOG_WARNING,
				_("Skipped overly long line %u"), line);
		}

		lbuf[len] = '\0';
		char nbuf[32], vbuf[1024];

		switch (sscanf (lbuf, " %31s %1023s", nbuf, vbuf))
		{
			case 2:
				if ((*nbuf != '#') // comment
				 && !Set (nbuf, vbuf, line))
					return false;
				break;

			case 1:
				if (*nbuf != '#')
					syslog (LOG_WARNING,
						_("Ignoring line %u: %s"),
						line, nbuf);
				break;
		}
	}

	if (ferror (stream))
	{
		syslog (LOG_ERR, _("Error reading configuration file: %m"));
		return false;
	}
	return true;
}


bool
MiredoConf::ReadFile (const char *path)
{
	FILE *stream = fopen (path, "r");
	if (stream != NULL)
	{
		bool ret = ReadFile (stream);
		fclose (stream);
		return ret;
	}
	syslog (LOG_ERR, _("Error opening configuration file %s: %m"), path);
	return false;
}


const char *
MiredoConf::GetRawValue (const char *name, unsigned *line)
{
	struct setting *prev = NULL;

	for (struct setting *p = head; p != NULL; p = p->next)
	{
		if (stricmp (p->name, name) == 0)
		{
			const char *buf = p->value;

			if (line != NULL)
				*line = p->line;

			if (prev != NULL)
				prev->next = p->next;
			else
				head = p->next;

			free (p);
			return buf;
		}
		prev = p;
	}

	return NULL;
}


bool
MiredoConf::GetString (const char *name, char **value, unsigned *line)
{
	const char *val = GetRawValue (name, line);
	if (val == NULL)
		return true;

	char *buf = strdup (val);
	if (buf == NULL)
		return false;

	*value = buf;
	return true;
}


bool
MiredoConf::GetInt16 (const char *name, uint16_t *value, unsigned *line)
{
	const char *val = GetRawValue (name, line);

	if (val == NULL)
		return true;

	char *end;
	unsigned long l;

	l = strtoul (val, &end, 0);
	
	if ((*end) || (l > 65535))
	{
		syslog (LOG_ERR,
			_("Invalid integer value \"%s\" for %s: %m"),
			val, name);
		return false;
	}
	*value = (uint16_t)l;
	return true;
}



/*
 * Returns an IPv4 address (network byte order) associated with hostname
 * <name>.
 */
bool
ParseIPv4 (MiredoConf& conf, const char *name, uint32_t *value)
{
	unsigned line;
	const char *val = conf.GetRawValue (name, &line);

	if (val == NULL)
		return true;

	struct addrinfo help, *res;

	memset (&help, 0, sizeof (help));
	help.ai_family = AF_INET;
	help.ai_socktype = SOCK_DGRAM;
	help.ai_protocol = IPPROTO_UDP;

	int check = getaddrinfo (val, NULL, &help, &res);

	if (check)
	{
		syslog (LOG_ERR, _("Invalid hostname \"%s\" at line %u: %s"),
			name, line, gai_strerror (check));
		return false;
	}

	*value = ((const struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
	freeaddrinfo (res);
	return true;
}


bool
ParseIPv6 (MiredoConf& conf, const char *name, struct in6_addr *value)
{
	unsigned line;
	const char *val = conf.GetRawValue (name, &line);

	if (val == NULL)
		return true;

	struct addrinfo help, *res;

	memset (&help, 0, sizeof (help));
	help.ai_family = PF_INET6;
	help.ai_socktype = SOCK_DGRAM;
	help.ai_protocol = IPPROTO_UDP;

	int check = getaddrinfo (val, NULL, &help, &res);

	if (check)
	{
		syslog (LOG_ERR, _("Invalid hostname \"%s\" at line %u: %s"),
			name, line, gai_strerror (check));
		return false;
	}

	memcpy (value,
		&((const struct sockaddr_in6*)(res->ai_addr))->sin6_addr,
		sizeof (struct in6_addr));

	freeaddrinfo (res);
	return true;
}


bool
ParseTeredoPrefix (MiredoConf& conf, const char *name, uint32_t *value)
{
	union teredo_addr addr;

	if (ParseIPv6 (conf, name, &addr.ip6))
	{
		if (!is_valid_teredo_prefix (addr.teredo.prefix))
		{
			syslog (LOG_ALERT,
				_("Invalid Teredo IPv6 prefix: %x::/32"),
				addr.teredo.prefix);
			return false;
		}

		*value = addr.teredo.prefix;
		return true;
	}
	return false;
}


bool ParseRelayType (MiredoConf& conf, const char *name, bool *enabled,
			bool *cone)
{
	unsigned line;
	const char *val = conf.GetRawValue (name, &line);

	if (val == NULL)
		return true;

	if (stricmp (val, "disabled") == 0)
		*enabled = false;
	else if (stricmp (val, "cone") == 0 || stricmp (val, "none") == 0)
		*enabled = *cone = true;
	else if (stricmp (val, "restricted") == 0)
	{
		*enabled = true;
		*cone = false;
	}
	else
	{
		syslog (LOG_ERR, _("Invalid relay type \"%s\" at line %u"),
			val, line);
		return false;
	}
	return true;
}