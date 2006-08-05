/*
 * $Id$
 *
 * Copyright (c) 2002-2003, Raphael Manfredi
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

/**
 * @ingroup core
 * @file
 *
 * HTTP routines.
 *
 * The whole HTTP logic is not contained here.  Only generic supporting
 * routines are here.
 *
 * @author Raphael Manfredi
 * @date 2002-2003
 */

#include "common.h"

RCSID("$Id$")

#include "nodes.h"
#include "http.h"
#include "sockets.h"
#include "bsched.h"
#include "ioheader.h"
#include "version.h"
#include "token.h"
#include "clock.h"

#include "lib/atoms.h"
#include "lib/getline.h"
#include "lib/glib-missing.h"
#include "lib/header.h"
#include "lib/tm.h"
#include "lib/walloc.h"

#include "if/gnet_property_priv.h"

#include "lib/override.h"			/* Must be the last header included */

http_url_error_t http_url_errno;	/**< Error from http_url_parse() */

static GSList *sl_outgoing = NULL;	/**< To spot reply timeouts */

/**
 * Send HTTP status on socket, with code and reason.
 *
 * If `hev' is non-null, it points to a vector of http_extra_desc_t items,
 * containing `hevcnt' entries.  Each entry describes something to be
 * inserted in the header.
 *
 * The connection is NOT closed physically.
 *
 * At the HTTP level, the connection is closed if an error is returned
 * (either 4xx or 5xx) or a redirection occurs (3xx).
 * Unless keep_alive = true.
 *
 * When the outgoing bandwidth is saturated, we start to limit the size of
 * the generated headers.  We reduce the size of the generated header
 * to about 512 bytes, and remove non-essential things.
 *
 * @returns TRUE if we were able to send everything, FALSE otherwise.
 */
gboolean
http_send_status(
	struct gnutella_socket *s, gint code, gboolean keep_alive,
	http_extra_desc_t *hev, gint hevcnt,
	const gchar *reason, ...)
{
	gchar header[2560];			/* 2.5 K max */
	gchar status_msg[512];
	gchar xlive[512];
	gint rw;
	gint mrw;
	gint sent;
	gint i;
	va_list args;
	const gchar *conn_close = keep_alive ? "" : "Connection: close\r\n";
	const gchar *no_content = "Content-Length: 0\r\n";
	const gchar *version;
	const gchar *date;
	const gchar *token;
	const gchar *body = NULL;
	gint header_size = sizeof(header);
	gboolean saturated = bsched_saturated(bws.out);
	gint cb_flags = 0;

	va_start(args, reason);
	gm_vsnprintf(status_msg, sizeof(status_msg)-1, reason, args);
	va_end(args);

	/*
	 * Prepare flags for callbacks.
	 */

	if (saturated)					cb_flags |= HTTP_CBF_BW_SATURATED;
	if (code == 503)				cb_flags |= HTTP_CBF_BUSY_SIGNAL;

	/*
	 * On 5xx errors, limit the header to 1K max, a priori.  This will be
	 * further reduced below if we have saturated the bandwidth.
	 * Likewise, on 4xx errors, we don't need to send much, excepted on 416:
	 * we need a longer reply when the connection is kept alive because of
	 * the available ranges to propagate.
	 */

	if		(code >= 500 && code <= 599)	header_size = 1024;
	else if	(code >= 400 && code <= 499)	header_size = 512;

	/*
	 * Activate X-Available-Ranges: emission on 416 and 2xx provided the
	 * connection will be kept alive.
	 */

	if (keep_alive) {
		if (code == 416) {
			header_size = sizeof(header);		/* Was reduced above for 4xx */
			cb_flags |= HTTP_CBF_SHOW_RANGES;
		} else if (code >= 200 && code <= 299)
			cb_flags |= HTTP_CBF_SHOW_RANGES;
	}

	/*
	 * If bandwidth is short, drop X-Live-Since, and reduce the header
	 * size noticeably, so that only the most important stuff gets out.
	 *		--RAM, 12/10/2003
	 */

	if (saturated && code >= 300) {
		xlive[0] = '\0';
		version = version_short_string;
		token = socket_omit_token(s) ? NULL : tok_short_version();
		header_size = 512;
		cb_flags |= HTTP_CBF_SMALL_REPLY;
	} else {
		gm_snprintf(xlive, sizeof(xlive)-1,
			"X-Live-Since: %s\r\n", start_rfc822_date);
		version = version_string;
		token = socket_omit_token(s) ? NULL : tok_version();
	}

	for (i = 0; i < hevcnt; i++) {
		http_extra_desc_t *he = &hev[i];

		if (HTTP_EXTRA_BODY == he->he_type) {
			if ('\0' != he->he_msg[0])
				body = he->he_msg;
			break;
		}
	}

	if (code < 300 || !keep_alive || body)
		no_content = "";

	g_assert((size_t) header_size <= sizeof header);

	date = timestamp_rfc1123_to_string(clock_loc2gmt(tm_time()));
	rw = gm_snprintf(header, header_size,
		"HTTP/1.1 %d %s\r\n"
		"Server: %s\r\n"
		"Date: %s\r\n"
		"%s"			/* Connection */
		"%s%s%s"		/* X-Token (optional) */
		"%s"			/* X-Live-Since */
		"%s",			/* Content length */
		code, status_msg, version, date, conn_close,
		token ? "X-Token: " : "",
		token ? token : "",
		token ? "\r\n" : "",
		xlive, no_content);

	mrw = rw;		/* Minimal header length */

	/*
	 * Append extra information to the minimal header created above.
	 */

	/*
	 * The +3 is there to leave room for "\r\n\0"
	 *		-- JA, 09/02/2004
	 */
	for (i = 0; i < hevcnt && rw + 3 < header_size; i++) {
		http_extra_desc_t *he = &hev[i];
		http_extra_type_t type = he->he_type;

		switch (type) {
		case HTTP_EXTRA_BODY:
			/* Already handled above */
			break;
		case HTTP_EXTRA_LINE:
			rw += gm_snprintf(&header[rw], header_size - rw, "%s", he->he_msg);
			break;
		case HTTP_EXTRA_CALLBACK:
			{
				gint len = header_size - rw;

				g_assert(header_size > rw);
				g_assert(len > 0);

				(*he->he_cb)(&header[rw], &len, he->he_arg, cb_flags);

				g_assert(len >= 0);
				g_assert(len + rw <= header_size);

				rw += len;
			}
			break;
		}
	}

	if (body)
		rw += gm_snprintf(&header[rw], header_size - rw,
						"Content-Length: %lu\r\n", (gulong) strlen(body));

	if (rw < header_size)
		rw += gm_snprintf(&header[rw], header_size - rw, "\r\n");

	if (body)
		rw += gm_snprintf(&header[rw], header_size - rw, "%s", body);

	if (rw >= header_size && hev) {
		g_warning("HTTP status %d (%s) too big, ignoring extra information",
			code, status_msg);

		rw = mrw + gm_snprintf(&header[mrw], header_size - mrw, "\r\n");
		g_assert(rw < header_size);
	}

	if (-1 == (sent = bws_write(bws.out, &s->wio, header, rw))) {
		socket_eof(s);
		if (http_debug > 1)
			g_warning("unable to send back HTTP status %d (%s) to %s: %s",
			code, status_msg, host_addr_to_string(s->addr), g_strerror(errno));
		return FALSE;
	} else if (sent < rw) {
		if (http_debug) g_warning(
			"only sent %d out of %d bytes of status %d (%s) to %s",
			sent, rw, code, status_msg, host_addr_to_string(s->addr));
		return FALSE;
	} else if (http_debug > 2) {
		g_message("----Sent HTTP Status to %s (%d bytes):\n%.*s----\n",
			host_addr_to_string(s->addr), rw, rw, header);
	}

	return TRUE;
}

/**
 * HTTP status callback.
 *
 * Add an X-Hostname line bearing the fully qualified hostname.
 */
void
http_hostname_add(gchar *buf, gint *retval, gpointer unused_arg, guint32 flags)
{
	size_t length = *retval;
	size_t rw;

	(void) unused_arg;
	g_assert(length <= INT_MAX);

	if (flags & HTTP_CBF_SMALL_REPLY)
		rw = 0;
	else
		rw = gm_snprintf(buf, length, "X-Hostname: %s\r\n", server_hostname);

	if (rw != length - 1)
		*retval = rw;
}

/***
 *** HTTP parsing.
 ***/

/**
 * Parse status messages formed of leading digit numbers, then an optional
 * message.  The pointer to the start of the message is returned in `msg'
 * if it is non-null.
 *
 * @return status code, -1 on error.
 */
static gint
code_message_parse(const gchar *line, const gchar **msg)
{
	const gchar *endptr;
	guint32 v;
	gint error;

	/*
	 * We expect exactly 3 status digits.
	 */

	v = parse_uint32(line, &endptr, 10, &error);
	if (error || v > 999 || (*endptr != '\0' && !is_ascii_space(*endptr)))
		return -1;

	if (msg)
		*msg = skip_ascii_spaces(endptr);

	return v;
}

/**
 * Parse protocol status line, and return the status code, and optionally a
 * pointer within the string where the status message starts (if `msg' is
 * a non-null pointer), and the protocol major/minor (if `major' and `minor'
 * are non-null).
 *
 * If `proto' is non-null, then when there is a leading protocol string in
 * the reply, it must be equal to `proto'.
 *
 * @returns -1 if it fails to parse the status line correctly, the status code
 * otherwise.
 *
 * We recognize the following status lines:
 *
 *     - ZZZ 403 message                        (major=0, minor=0)
 *     - ZZZ/2.3 403 message                    (major=2, minor=3)
 *     - 403 message                            (major=0, minor=0)
 *
 * We don't yet handle "SMTP-like continuations":
 *
 *     - 403-message line #1
 *     - 403-message line #2
 *     - 403 last message line
 *
 * There is no way to return the value of "ZZZ" via this routine.
 *
 * @attention
 * NB: this routine is also used to parse GNUTELLA status codes, since
 * they follow the same pattern as HTTP status codes.
 */
gint
http_status_parse(const gchar *line,
	const gchar *proto, const gchar **msg, guint *major, guint *minor)
{
	guchar c;
	const gchar *p;

	/*
	 * Skip leading spaces.
	 */

	p = skip_ascii_spaces(line);
	c = *p;

	/*
	 * If first character is a digit, then we have simply:
	 *
	 *   403 message
	 *
	 * There's no known protocol information.
	 */

	if (c == '\0')
		return -1;					/* Empty line */

	if (is_ascii_digit(c)) {
		if (major)
			*major = 0;
		if (minor)
			*minor = 0;
		return code_message_parse(p, msg);
	}

	/*
	 * Check protocol.
	 */

	if (proto) {
		if (NULL != (p = is_strprefix(line, proto))) {
			/*
			 * Protocol string matches, make sure it ends with a space or
			 * a "/" delimiter.
			 */

			c = *p;					/* Can dereference, at worst it's a NUL */
			if (c == '\0')			/* Only "protocol" name in status */
				return -1;
			if (!is_ascii_space(c) && c != '/')
				return -1;
		} else
			return -1;
	} else {
		/*
		 * Move along the string until we find a space or a "/".
		 */

		for (/* empty */; c; c = *(++p)) {
			if (c == '/' || is_ascii_space(c))
				break;
		}
	}

	if (c == '\0')
		return -1;

	/*
	 * We've got a "/", parse protocol version number, then move past
	 * to the first space.
	 */

	if (c == '/') {
		if (major || minor) {
			if (0 != parse_major_minor(&p[1], NULL, major, minor))
				return -1;
		}

		for (c = *(++p); c; c = *(++p)) {
			if (is_ascii_space(c))
				break;
		}

		if (c == '\0')
			return -1;
	}

	g_assert(is_ascii_space(c));

	/*
	 * Now strip leading spaces.
	 */

	p = skip_ascii_spaces(++p);
	c = *p;

	if (c == '\0' || !is_ascii_digit(c))
		return -1;

	return code_message_parse(p, msg);
}


/**
 * Extract HTTP version major/minor out of the given request, whose string
 * length is `len' bytes.
 *
 * @returns TRUE when we identified the "HTTP/x.x" trailing string, filling
 * major and minor accordingly.
 */
gboolean
http_extract_version(
	const gchar *request, size_t len, guint *major, guint *minor)
{
	const gchar *p;
	size_t limit, i;

	/*
	 * The smallest request would be "X / HTTP/1.0".
	 */

	limit = sizeof("X / HTTP/1.0") - 1;

	if (http_debug > 4)
		printf("HTTP req (%lu bytes): %s\n", (gulong) len, request);

	if (len < limit)
		return FALSE;

	/*
	 * Scan backwards, until we find the first space with the last trailing
	 * chars.  If we don't, it can't be an HTTP request.
	 */

	for (p = &request[len - 1], i = 0; i < limit; p--, i++) {
		if (' ' == *p)		/* Not isspace(), looking for space only */
			break;
	}

	if (http_debug > 4)
		g_message("HTTP i = %lu, limit = %lu", (gulong) i, (gulong) limit);

	if (i == limit)
		return FALSE;		/* Reached our limit without finding a space */

	/*
	 * Here, `p' point to the space character.
	 */

	g_assert(*p == ' ');
	p++;

	if (
		NULL == (p = is_strprefix(p, "HTTP/")) ||
		0 != parse_major_minor(p, NULL, major, minor)
	) {
		if (http_debug > 1)
			printf("HTTP req (%lu bytes): no protocol tag: %s\n",
				(gulong) len, request);
		return FALSE;
	}

	if (http_debug > 4)
		printf("HTTP req OK (%u.%u)\n", *major, *minor);

	/*
	 * We don't check trailing chars after the HTTP/x.x indication.
	 * There should not be any, but even if there are, we'll just ignore them.
	 */

	return TRUE;			/* Parsed HTTP/x.x OK */
}

/***
 *** HTTP URL parsing.
 ***/

static gchar *parse_errstr[] = {
	"OK",								/**< HTTP_URL_OK */
	"Not an http URI",					/**< HTTP_URL_NOT_HTTP */
	"More than one <user>:<password>",	/**< HTTP_URL_MULTIPLE_CREDENTIALS */
	"Truncated <user>:<password>",		/**< HTTP_URL_BAD_CREDENTIALS */
	"Could not parse port",				/**< HTTP_URL_BAD_PORT_PARSING */
	"Port value is out of range",		/**< HTTP_URL_BAD_PORT_RANGE */
	"Could not parse host",				/**< HTTP_URL_BAD_HOST_PART */
	"Could not resolve host into IP",	/**< HTTP_URL_HOSTNAME_UNKNOWN */
	"URL has no URI part",				/**< HTTP_URL_MISSING_URI */
};

/**
 * @return human-readable error string corresponding to error code `errnum'.
 */
const gchar *
http_url_strerror(http_url_error_t errnum)
{
	if ((gint) errnum < 0 || errnum >= G_N_ELEMENTS(parse_errstr))
		return "Invalid error code";

	return parse_errstr[errnum];
}

/**
 * Parse HTTP url and extract the IP/port we need to connect to.
 * Also identifies the start of the path to request on the server.
 *
 * @returns TRUE if the URL was correctly parsed, with `port', 'host'
 * and `path' filled if they are non-NULL, FALSE otherwise.
 * The variable `http_url_errno' is set accordingly.
 *
 */
gboolean
http_url_parse(const gchar *url, guint16 *port, const gchar **host,
	const gchar **path)
{
	static gchar hostname[MAX_HOSTLEN + 1];
	struct {
		const gchar *host, *path;
		guint16 port;
	} tmp;
	const gchar *endptr, *p;
	host_addr_t addr;

	g_assert(url);

	if (!host) host = &tmp.host;
	if (!path) path = &tmp.path;
	if (!port) port = &tmp.port;

	/*
	 * The general URL syntax is (RFC 1738):
	 *
	 *	//[<user>[:<pass>]@]<host>[:<port>]/[<url-path>]
	 *
	 */

	/* Assume there's no <user>:<password> */
	p = is_strcaseprefix(url, "http://");
	if (!p) {
		http_url_errno = HTTP_URL_NOT_HTTP;
		return FALSE;
	}

	/*
	 * Extract hostname into hostname[].
	 */

	if (!string_to_host_or_addr(p, &endptr, &addr)) {
		http_url_errno = HTTP_URL_BAD_HOST_PART;
		return FALSE;
	}

	if (is_host_addr(addr)) {
		host_addr_to_string_buf(addr, hostname, sizeof hostname);
	} else {
		size_t len;

		len = endptr - p;
		if (len >= sizeof hostname) {
			http_url_errno = HTTP_URL_BAD_HOST_PART;
			return FALSE;
		}
		memcpy(hostname, p, len);
		hostname[len] = '\0';
	}
	p = endptr;
	*host = hostname;				/* Static data! */

	if (':' != *p) {
		*port = HTTP_PORT;
	} else {
		gint error;
		guint32 u;

		g_assert(':'== *p);
		p++;

		u = parse_uint32(p, &endptr, 10, &error);
		if (error) {
			http_url_errno = HTTP_URL_BAD_PORT_PARSING;
			return FALSE;
		} else if (u > 65535) {
			http_url_errno = HTTP_URL_BAD_PORT_RANGE;
			return FALSE;
		}
		p = endptr;
		*port = u;
	}

	*path = p;
	if ('/' != *p) {
		http_url_errno = HTTP_URL_MISSING_URI;
		return FALSE;
	}

	if (http_debug > 4) {
		g_message("URL \"%s\" -> host=\"%s\", port=%u, path=\"%s\"\n",
			url, *host, (unsigned) *port, *path);
	}

	http_url_errno = HTTP_URL_OK;

	return TRUE;
}

/***
 *** HTTP buffer management.
 ***/

/**
 * Allocate HTTP buffer, capable of holding data at `buf', of `len' bytes,
 * and whose `written' bytes have already been sent out.
 */
http_buffer_t *
http_buffer_alloc(gchar *buf, size_t len, size_t written)
{
	http_buffer_t *b;

	g_assert(buf);
	g_assert(len > 0 && len <= INT_MAX);
	g_assert((gint) written >= 0 && written < len);

	b = walloc(sizeof(*b));
	b->hb_arena = walloc(len);		/* Should be small enough for walloc */
	b->hb_len = len;
	b->hb_end = b->hb_arena + len;
	b->hb_rptr = b->hb_arena + written;

	memcpy(b->hb_arena, buf, len);

	return b;
}

/**
 * Dispose of HTTP buffer.
 */
void
http_buffer_free(http_buffer_t *b)
{
	g_assert(b);

	wfree(b->hb_arena, b->hb_len);
	wfree(b, sizeof(*b));
}

/**
 * Parses the content of a Content-Range header.
 *
 * @param buf should point the payload of a Content-Range header
 * @param start will be set to the ``start'' offset
 * @param end will be set to the ``end'' offset
 * @param total will be set to the ``total'' size of the requested object.
 *
 * @return -1 on error, zero on success.
 */
gint
http_content_range_parse(const gchar *buf,
		filesize_t *start, filesize_t *end, filesize_t *total)
{
	const gchar *s = buf, *endptr;
	gint error;

	/*
	 * HTTP/1.1 -- RFC 2616 -- 3.12 Range Units
	 *
	 *		bytes SP start '-' end '/' total
	 *
	 * HTTP/1.1 -- RFC 2616 -- 14.35.1 Byte Ranges
	 *
	 * This is wrong but used by some (legacy?) servers:
	 *
	 *		bytes '=' start '-' end '/' total
	 */

	s = is_strcaseprefix(s, "bytes");
	if (!s)
		return -1;

	if (*s != ' ' && *s != '=')
		return -1;

	s++;
	s = skip_ascii_spaces(s);
	*start = parse_uint64(s, &endptr, 10, &error);
	if (error || *endptr++ != '-')
		return -1;

	s = skip_ascii_spaces(endptr);
	*end = parse_uint64(s, &endptr, 10, &error);
	if (error || *endptr++ != '/')
		return -1;

	s = skip_ascii_spaces(endptr);
	*total = parse_uint64(s, &endptr, 10, &error);

	/*
	 * According to the HTTP/1.1 specs, start <= end < total
	 * must be true, otherwise the range is invalid.
	 */

	if (error || *start > *end || *end >= *total)
		return -1;

	return 0;
}

/***
 *** HTTP range parsing.
 ***/

/**
 * Add a new http_range_t object within the sorted list.
 *
 * Refuse to add the range if it is overlapping existing ranges.
 *
 * @param `list' must be sorted if not NULL.
 * @param `start' the start of the range to add
 * @param `end' the end of the range to add
 * @param `field' arguments are only there to log errors, if any.
 * @param `vendor' is same as `field'.
 * @param `ignored' is set to TRUE if range was ignored.
 *
 * @return the new head of the list.
 */
static GSList *
http_range_add(GSList *list, filesize_t start, filesize_t end,
	const gchar *field, const gchar *vendor, gboolean *ignored)
{
	GSList *l;
	GSList *prev;
	http_range_t *item;

	g_assert(start <= end);		/* 0-0 is a 1-byte range containing byte 0 */

	item = walloc(sizeof(*item));
	item->start = start;
	item->end = end;

	*ignored = FALSE;

	for (l = list, prev = NULL; l; prev = l, l = g_slist_next(l)) {
		http_range_t *r = (http_range_t *) l->data;

		/*
		 * The list is sorted and there should be no overlapping between
		 * the items, so as soon as we find a range that starts after "end",
		 * we know we have to insert before.
		 */

		if (r->start > end) {
			GSList *next;

			/* Ensure range is not overlapping with previous */
			if (prev != NULL) {
				http_range_t *pr = (http_range_t *) prev->data;

				if (pr->end >= start) {
					gchar start_buf[UINT64_DEC_BUFLEN];
					gchar end_buf[UINT64_DEC_BUFLEN];

					uint64_to_string_buf(start, start_buf, sizeof start_buf);
					uint64_to_string_buf(end, end_buf, sizeof end_buf);

					g_warning("vendor <%s> sent us overlapping range %s-%s "
						"(with previous %s-%s) in the %s header -- ignoring",
						vendor, start_buf, end_buf,
						uint64_to_string(pr->start),
						uint64_to_string2(pr->end),
						field);
					goto ignored;
				}
			}

			/* Ensure range is not overlapping with next, if any */
			next = g_slist_next(l);
			if (next != NULL) {
				http_range_t *nr = (http_range_t *) next->data;
				if (nr->start <= end) {
					gchar start_buf[UINT64_DEC_BUFLEN];
				   	gchar end_buf[UINT64_DEC_BUFLEN];

					uint64_to_string_buf(start, start_buf, sizeof start_buf);
					uint64_to_string_buf(end, end_buf, sizeof end_buf);

					g_warning("vendor <%s> sent us overlapping range %s-%s "
						"(with next %s-%s) in the %s header -- ignoring",
						vendor, start_buf, end_buf,
						uint64_to_string(nr->start),
						uint64_to_string2(nr->end),
						field);
					goto ignored;
				}
			}

			/* Insert after `prev' (which may be NULL) */
			return gm_slist_insert_after(list, prev, item);
		}

		if (r->end >= start) {
			gchar start_buf[UINT64_DEC_BUFLEN];
			gchar end_buf[UINT64_DEC_BUFLEN];

			uint64_to_string_buf(start, start_buf, sizeof start_buf);
			uint64_to_string_buf(end, end_buf, sizeof end_buf);

			g_warning("vendor <%s> sent us overlapping range %s-%s "
				"(with %s-%s) in the %s header -- ignoring",
				vendor, start_buf, end_buf, uint64_to_string(r->start),
				uint64_to_string2(r->end), field);
			goto ignored;
		}
	}

	/*
	 * Insert at the tail of the list.
	 *
	 * NB: the following call works as expected is list == NULL, because
	 * then prev == NULL and we insert `item' as the first and only entry.
	 */

	return gm_slist_insert_after(list, prev, item);

ignored:
	*ignored = TRUE;
	wfree(item, sizeof(*item));		/* Item was not inserted */
	return list;					/* No change in list */
}

/**
 * Parse a Range: header in the request, returning the list of ranges
 * that are enumerated.  Invalid ranges are ignored.
 *
 * Only "bytes" ranges are supported.
 *
 * When parsing a "bytes=" style, it means it's a request, so we allow
 * negative ranges.  Otherwise, for "bytes " specifications, it's a reply
 * and we ignore negative ranges.
 *
 * `size' gives the length of the resource, to resolve negative ranges and
 * make sure we don't have ranges that extend past that size.
 *
 * The `field' and `vendor' arguments are only there to log errors, if any.
 *
 * @returns a sorted list of http_range_t objects.
 */
GSList *
http_range_parse(
	const gchar *field, const gchar *value, filesize_t size,
	const gchar *vendor)
{
	static const gchar unit[] = "bytes";
	GSList *ranges = NULL;
	const gchar *str = value;
	guchar c;
	filesize_t start;
	filesize_t end;
	gboolean request = FALSE;		/* True if 'bytes=' is seen */
	gboolean has_start;
	gboolean has_end;
	gboolean skipping;
	gboolean minus_seen;
	gboolean ignored;
	gint count = 0;

	g_assert(size > 0);

	if (NULL != (str = is_strprefix(str, unit))) {
		c = *str;
		if (!is_ascii_space(c) && c != '=') {
			g_warning("improper %s header from <%s>: %s", field, vendor, value);
			return NULL;
		}
	} else {
		g_warning("improper %s header from <%s> (not bytes?): %s",
			field, vendor, value);
		return NULL;
	}

	/*
	 * Move to the first non-space char.
	 * Meanwhile, if we see a '=', we know it's a request-type range header.
	 */

	while ((c = *str)) {
		if (c == '=') {
			if (request) {
				g_warning("improper %s header from <%s> (multiple '='): %s",
					field, vendor, value);
				return NULL;
			}
			request = TRUE;
			str++;
			continue;
		}
		if (is_ascii_space(c)) {
			str++;
			continue;
		}
		break;
	}

	start = 0;
	has_start = FALSE;
	has_end = FALSE;
	end = size - 1;
	skipping = FALSE;
	minus_seen = FALSE;

	while ((c = *str++)) {
		if (is_ascii_space(c))
			continue;

		if (c == ',') {
			if (skipping) {
				skipping = FALSE;		/* ',' is a resynch point */
				continue;
			}

			if (!minus_seen) {
				if (http_debug) g_warning(
					"weird %s header from <%s>, offset %d (no range?): "
					"%s", field, vendor, (gint) (str - value) - 1, value);
				goto reset;
			}

			if (start == HTTP_OFFSET_MAX && !has_end) {	/* Bad negative range */
				if (http_debug) g_warning(
					"weird %s header from <%s>, offset %d "
					"(incomplete negative range): %s",
					field, vendor, (gint) (str - value) - 1, value);
				goto reset;
			}

			if (start > end) {
				if (http_debug) g_warning(
					"weird %s header from <%s>, offset %d "
					"(swapped range?): %s", field, vendor,
					(gint) (str - value) - 1, value);
				goto reset;
			}

			ranges = http_range_add(ranges,
				start, end, field, vendor, &ignored);
			count++;

			if (ignored) {
				if (http_debug) g_warning(
					"weird %s header from <%s>, offset %d "
					"(ignored range #%d): %s",
					field, vendor, (gint) (str - value) - 1, count,
					value);
			}

			goto reset;
		}

		if (skipping)				/* Waiting for a ',' */
			continue;

		if (c == '-') {
			if (minus_seen) {
				if (http_debug) g_warning(
					"weird %s header from <%s>, offset %d (spurious '-'): %s",
					field, vendor, (gint) (str - value) - 1, value);
				goto resync;
			}
			minus_seen = TRUE;
			if (!has_start) {		/* Negative range */
				if (!request) {
					if (http_debug)
						g_warning("weird %s header from <%s>, offset %d "
							"(negative range in reply): %s",
							field, vendor, (gint) (str - value) - 1, value);
					goto resync;
				}
				start = HTTP_OFFSET_MAX;	/* Indicates negative range */
				has_start = TRUE;
			}
			continue;
		}

		if (is_ascii_digit(c)) {
			gint error;
			const gchar *dend;
			guint64 val = parse_uint64(str - 1, &dend, 10, &error);

			/* Started with digit! */
			g_assert(dend != (str - 1));

			str = dend;		/* Skip number */

			if (has_end) {
				if (http_debug)
					g_warning("weird %s header from <%s>, offset %d "
						"(spurious boundary %s): %s",
						field, vendor, (gint) (str - value) - 1,
						uint64_to_string(val), value);
				goto resync;
			}

			if (val >= size) {
				/* ``last-byte-pos'' may extend beyond the actual
				 * filesize. It's more a response limit than an exact
				 * range end specifier.
				 */
				val = size - 1;
			}

			if (has_start) {
				if (!minus_seen) {
					if (http_debug)
						g_warning("weird %s header from <%s>, offset %d "
							"(no '-' before boundary %s): %s",
							field, vendor, (gint) (str - value) - 1,
							uint64_to_string(val), value);
					goto resync;
				}
				if (start == HTTP_OFFSET_MAX) {			/* Negative range */
					start = (val > size) ? 0 : size - val;	/* Last bytes */
					end = size - 1;
				} else {
					end = val;
				}
				has_end = TRUE;
			} else {
				start = val;
				has_start = TRUE;
			}
			continue;
		}

		if (http_debug) g_warning("weird %s header from <%s>, offset %d "
			"(unexpected char '%c'): %s",
			field, vendor, (gint) (str - value) - 1, c, value);

		/* FALL THROUGH */

	resync:
		skipping = TRUE;
	reset:
		start = 0;
		has_start = FALSE;
		has_end = FALSE;
		minus_seen = FALSE;
		end = size - 1;
	}

	/*
	 * Handle trailing range, if needed.
	 */

	if (minus_seen) {
		if (start == HTTP_OFFSET_MAX && !has_end) {	/* Bad negative range */
			if (http_debug) g_warning("weird %s header from <%s>, offset %d "
				"(incomplete trailing negative range): %s",
				field, vendor, (gint) (str - value) - 1, value);
			goto final;
		}

		if (start > end) {
			if (http_debug) g_warning("weird %s header from <%s>, offset %d "
				"(swapped trailing range?): %s", field, vendor,
				(gint) (str - value) - 1, value);
			goto final;
		}

		ranges = http_range_add(ranges, start, end, field, vendor, &ignored);
		count++;

		if (ignored)
			if (http_debug) g_warning("weird %s header from <%s>, offset %d "
				"(ignored final range #%d): %s",
				field, vendor, (gint) (str - value) - 1, count,
				value);
	}

final:

	if (http_debug > 4) {
		GSList *l;
		printf("Saw %d ranges in %s %s: %s\n",
			count, request ? "request" : "reply", field, value);
		if (ranges)
			printf("...retained:\n");
		for (l = ranges; l; l = g_slist_next(l)) {
			http_range_t *r = (http_range_t *) l->data;
			printf("...  %s-%s\n",
				uint64_to_string(r->start), uint64_to_string2(r->end));
		}
	}

	if (ranges == NULL && http_debug)
		g_warning("retained no ranges in %s header from <%s>: %s",
			field, vendor, value);

	return ranges;
}

/**
 * Free list of http_range_t objects.
 */
void
http_range_free(GSList *list)
{
	GSList *l;

	for (l = list; l; l = g_slist_next(l))
		wfree(l->data, sizeof(http_range_t));

	g_slist_free(list);
}

/**
 * @returns total size of all the ranges.
 */
filesize_t
http_range_size(const GSList *list)
{
	const GSList *l;
	filesize_t size = 0;

	for (l = list; l; l = g_slist_next(l)) {
		http_range_t *r = l->data;
		size += r->end - r->start + 1;
	}

	return size;
}

/**
 * @returns a pointer to static data, containing the available ranges.
 */
const gchar *
http_range_to_string(const GSList *list)
{
	static gchar str[4096];
	const GSList *sl = list;
	size_t rw;

	for (rw = 0; sl && (size_t) rw < sizeof(str); sl = g_slist_next(sl)) {
		const http_range_t *r = (const http_range_t *) sl->data;
		gchar start_buf[UINT64_DEC_BUFLEN], end_buf[UINT64_DEC_BUFLEN];

		uint64_to_string_buf(r->start, start_buf, sizeof start_buf);
		uint64_to_string_buf(r->end, end_buf, sizeof end_buf);
		rw += gm_snprintf(&str[rw], sizeof(str)-rw, "%s-%s",
				start_buf, end_buf);

		if (g_slist_next(sl) != NULL)
			rw += gm_snprintf(&str[rw], sizeof(str)-rw, ", ");
	}

	return str;
}

/**
 * Checks whether range contains the contiguous [from, to] interval.
 */
gboolean
http_range_contains(GSList *ranges, filesize_t from, filesize_t to)
{
	GSList *l;

	/*
	 * The following relies on the fact that the `ranges' list is sorted
	 * and that it contains disjoint intervals.
	 */

	for (l = ranges; l; l = g_slist_next(l)) {
		http_range_t *r = (http_range_t *) l->data;

		if (from > r->end)
			continue;

		if (from < r->start)
			break;			/* `from' outside of any following interval */

		/* `from' is within `r' */

		if (to <= r->end)
			return TRUE;

		break;				/* No other interval can contain `from' */
	}

	return FALSE;
}

/**
 * @returns a new copy of the given HTTP range.
 */
static http_range_t *
http_range_clone(http_range_t *range)
{
	http_range_t *r;

	r = walloc(sizeof(*r));
	r->start = range->start;
	r->end = range->end;

	return r;
}

/**
 * @returns a new list based on the merged ranges in the other lists given.
 */
GSList *
http_range_merge(GSList *old_list, GSList *new_list)
{
	http_range_t *old_range;
	http_range_t *new_range;
	http_range_t *r;
	GSList *new = new_list;
	GSList *old = old_list;
	GSList *result_list = NULL;
	filesize_t highest = 0;


	/*
	 * Build a result list based on the data in the old and new
	 * lists.
	 */

	while (old || new) {
		if (old && new) {
			old_range = (http_range_t *) old->data;
			new_range = (http_range_t *) new->data;

			/*
			 * If ranges are identical just copy one.
			 */

			if (new_range->start == old_range->start
				&& new_range->end == old_range->end) {
				highest = old_range->end;
				result_list =
					g_slist_append(result_list, http_range_clone(old_range));
				old = g_slist_next(old);
				new = g_slist_next(new);
				continue;
			}

			/*
			 * Skip over any ranges now below the highest mark, they
			 * are no longer relevant.
			 */

			if (old_range->end < highest) {
				old = g_slist_next(old);
				continue;
			}
			if (new_range->end < highest) {
				new = g_slist_next(new);
				continue;
			}

			/*
			 * First handle the non-overlapping case. Copy the first
			 * non-overlapping range, and move to the next range in
			 * that list.
			 */

			if (new_range->end < old_range->start) {
				highest = new_range->end;
				result_list =
					g_slist_append(result_list, http_range_clone(new_range));
				new = g_slist_next(new);
				continue;
			}
			if (old_range->end < new_range->start) {
				highest = new_range->end;
				result_list =
					g_slist_append(result_list, http_range_clone(old_range));

				old = g_slist_next(old);
				continue;
			}

			/*
			 * Handle overlapping case. Define a new range based on
			 * boundaries of both ranges, add it, and then move to
			 * next on both lists. We don't need to worry about
			 * non-overlapping case here because we handled that just
			 * before.
			 */

			if (new_range->start > old_range->start) {
				r = walloc(sizeof(*r));
				r->start = old_range->start;
				if (new_range->end > old_range->end)
					r->end = new_range->end;
				else
					r->end = old_range->end;
				highest = r->end;
				result_list = g_slist_append(result_list, r);
				old = g_slist_next(old);
				new = g_slist_next(new);
				continue;
			}
			if (new_range->start <= old_range->start) {
				r = walloc(sizeof(*r));
				r->start = new_range->start;
				if (new_range->end > old_range->end)
					r->end = new_range->end;
				else
					r->end = old_range->end;
				highest = r->end;
				result_list = g_slist_append(result_list, r);
				old = g_slist_next(old);
				new = g_slist_next(new);
				continue;
			}

		} else {

			/*
			 * If there are no chunks left in one of the lists we just
			 * copy the other ones unless they are below the highest mark.
			 */

			if (old) {
				old_range = (http_range_t *) old->data;
				if (old_range->end > highest)
					result_list = g_slist_append(result_list,
								  http_range_clone(old_range));
				old = g_slist_next(old);
			}
			if (new) {
				new_range = (http_range_t *) new->data;
				if (new_range->end > highest)
					result_list = g_slist_append(result_list,
								  http_range_clone(new_range));
				new = g_slist_next(new);
			}
		}
	}

	return result_list;
}



/***
 *** Asynchronous HTTP error code management.
 ***/

static const gchar * const error_str[] = {
	"OK",									/**< HTTP_ASYNC_OK */
	"Invalid HTTP URL",						/**< HTTP_ASYNC_BAD_URL */
	"Connection failed",					/**< HTTP_ASYNC_CONN_FAILED */
	"I/O error",							/**< HTTP_ASYNC_IO_ERROR */
	"Request too large",					/**< HTTP_ASYNC_REQ2BIG */
	"Header too large",						/**< HTTP_ASYNC_HEAD2BIG */
	"User cancel",							/**< HTTP_ASYNC_CANCELLED */
	"Got EOF",								/**< HTTP_ASYNC_EOF */
	"Unparseable HTTP status",				/**< HTTP_ASYNC_BAD_STATUS */
	"Got moved status, but no location",	/**< HTTP_ASYNC_NO_LOCATION */
	"Connection timeout",					/**< HTTP_ASYNC_CONN_TIMEOUT */
	"Data timeout",							/**< HTTP_ASYNC_TIMEOUT */
	"Nested redirection",					/**< HTTP_ASYNC_NESTED */
	"Invalid URI in Location header",		/**< HTTP_ASYNC_BAD_LOCATION_URI */
	"Connection was closed, all OK",		/**< HTTP_ASYNC_CLOSED */
	"Redirected, following disabled",		/**< HTTP_ASYNC_REDIRECTED */
};

guint http_async_errno;		/**< Used to return error codes during setup */

/**
 * @return human-readable error string corresponding to error code `errnum'.
 */
const gchar *
http_async_strerror(guint errnum)
{
	if (errnum >= G_N_ELEMENTS(error_str))
		return "Invalid error code";

	return error_str[errnum];
}

/***
 *** Asynchronous HTTP transactions.
 ***/

enum http_reqtype {
	HTTP_HEAD = 0,
	HTTP_GET,
	HTTP_POST,

	NUM_HTTP_REQTYPES
};

static const gchar * const http_verb[NUM_HTTP_REQTYPES] = {
	"HEAD",
	"GET",
	"POST",
};

#define HTTP_ASYNC_MAGIC 0xa91cf3eeU

/**
 * An asynchronous HTTP request.
 */
struct http_async {
	guint magic;					/**< Magic number */
	enum http_reqtype type;			/**< Type of request */
	http_state_t state;				/**< Current request state */
	guint32 flags;					/**< Operational flags */
	gchar *url;						/**< Initial URL request (atom) */
	gchar *path;					/**< Path to request (atom) */
	gchar *host;					/**< Hostname, if not a numeric IP (atom) */
	struct gnutella_socket *socket;	/**< Attached socket */
	http_header_cb_t header_ind;	/**< Callback for headers */
	http_data_cb_t data_ind;		/**< Callback for data */
	http_error_cb_t error_ind;		/**< Callback for errors */
	http_state_change_t state_chg;	/**< Optional: callback for state changes */
	time_t last_update;				/**< Time of last activity */
	gpointer io_opaque;				/**< Opaque I/O callback information */
	bio_source_t *bio;				/**< Bandwidth-limited source */
	gpointer user_opaque;			/**< User opaque data */
	http_user_free_t user_free;		/**< Free routine for opaque data */
	struct http_async *parent;		/**< Parent request, for redirections */
	http_buffer_t *delayed;			/**< Delayed data that could not be sent */
	gboolean allow_redirects;		/**< Whether we can follow HTTP redirects */
	GSList *children;				/**< Child requests */

	/*
	 * Operations that may be redefined by user.
	 */

	http_op_request_t op_request;	/**< Creates HTTP request */
};

/*
 * Operational flags.
 */

#define HA_F_FREED		0x00000001	/**< Structure has been logically freed */
#define HA_F_SUBREQ		0x00000002	/**< Children request now has control */

/**
 * In order to allow detection of logically freed structures when we return
 * from user callbacks, we delay the physical removal of the http_async
 * structure to the clock timer.  A freed structure is marked HA_F_FREED
 * and its magic number is zeroed to prevent accidental reuse.
 *
 * All freed structures are enqueued in the sl_ha_freed list.
 */

static GSList *sl_ha_freed = NULL;		/* Pending physical removal */

/**
 * Get URL and request information, given opaque handle.
 * This can be used by client code to log request parameters.
 *
 * @returns URL and fills `req' with the request type string (GET, POST, ...)
 * if it is a non-NULL pointer, `path' with the request path, `addr' and `port'
 * with the server address/port.
 */
const gchar *
http_async_info(
	gpointer handle, const gchar **req, gchar **path,
	host_addr_t *addr, guint16 *port)
{
	struct http_async *ha = handle;

	g_assert(ha->magic == HTTP_ASYNC_MAGIC);

	if (req)  *req  = http_verb[ha->type];
	if (path) *path = ha->path;
	if (addr) *addr   = ha->socket->addr;
	if (port) *port = ha->socket->port;

	return ha->url;
}

/**
 * Set user-defined opaque data, which can optionally be freed via `fn' if a
 * non-NULL function pointer is given.
 */
void
http_async_set_opaque(gpointer handle, gpointer data, http_user_free_t fn)
{
	struct http_async *ha = (struct http_async *) handle;

	g_assert(ha->magic == HTTP_ASYNC_MAGIC);
	g_assert(data != NULL);

	ha->user_opaque = data;
	ha->user_free = fn;
}

/**
 * Retrieve user-defined opaque data.
 */
gpointer
http_async_get_opaque(gpointer handle)
{
	struct http_async *ha = (struct http_async *) handle;

	g_assert(ha->magic == HTTP_ASYNC_MAGIC);

	return ha->user_opaque;
}

/**
 * Free this HTTP asynchronous request handler, disposing of all its
 * attached resources, recursively.
 */
static void
http_async_free_recursive(struct http_async *ha)
{
	GSList *l;

	g_assert(ha->magic == HTTP_ASYNC_MAGIC);

	g_assert(sl_outgoing);

	atom_str_free(ha->url);
	atom_str_free(ha->path);

	if (ha->host)
		atom_str_free(ha->host);
	if (ha->io_opaque)
		io_free(ha->io_opaque);
	if (ha->bio)
		bsched_source_remove(ha->bio);
	socket_free_null(&ha->socket);
	if (ha->user_free)
		(*ha->user_free)(ha->user_opaque);
	if (ha->delayed)
		http_buffer_free(ha->delayed);

	sl_outgoing = g_slist_remove(sl_outgoing, ha);

	/*
	 * Recursively free the children requests.
	 */

	for (l = ha->children; l; l = l->next) {
		struct http_async *cha = (struct http_async *) l->data;
		http_async_free_recursive(cha);
	}

	ha->magic = 0;					/* Prevent accidental reuse */
	ha->flags |= HA_F_FREED;		/* Will be freed later */
	ha->state = HTTP_AS_REMOVED;	/* Don't notify about state change! */

	sl_ha_freed = g_slist_prepend(sl_ha_freed, ha);
}

/**
 * Free the root of the HTTP asynchronous request handler, disposing
 * of all its attached resources.
 */
static void
http_async_free(struct http_async *ha)
{
	struct http_async *hax;

	g_assert(sl_outgoing);

	/*
	 * Find the root of the hierearchy into `hax'.
	 */

	for (hax = ha;; hax = hax->parent) {
		if (!hax->parent)
			break;
	}

	g_assert(hax != NULL);
	g_assert(hax->parent == NULL);

	http_async_free_recursive(hax);
}

/**
 * Free all structures that have already been logically freed.
 */
static void
http_async_free_pending(void)
{
	GSList *l;

	for (l = sl_ha_freed; l; l = l->next) {
		struct http_async *ha = (struct http_async *) l->data;

		g_assert(ha->flags & HA_F_FREED);
		wfree(ha, sizeof(*ha));
	}

	g_slist_free(sl_ha_freed);
	sl_ha_freed = NULL;
}

/**
 * Close request.
 */
void
http_async_close(gpointer handle)
{
	struct http_async *ha = (struct http_async *) handle;

	g_assert(ha->magic == HTTP_ASYNC_MAGIC);

	http_async_free(ha);
}

/**
 * Cancel request (internal call).
 */
static void
http_async_remove(gpointer handle, http_errtype_t type, gpointer code)
{
	struct http_async *ha = (struct http_async *) handle;

	g_assert(ha->magic == HTTP_ASYNC_MAGIC);

	(*ha->error_ind)(handle, type, code);
	http_async_free(ha);
}

/**
 * Cancel request (user request).
 */
void
http_async_cancel(gpointer handle)
{
	http_async_remove(handle, HTTP_ASYNC_ERROR, (gpointer) HTTP_ASYNC_CANCELLED);
}

/**
 * Cancel request (internal error).
 */
void
http_async_error(gpointer handle, gint code)
{
	http_async_remove(handle, HTTP_ASYNC_ERROR, GINT_TO_POINTER(code));
}

/**
 * Cancel request (system call error).
 */
static void
http_async_syserr(gpointer handle, gint code)
{
	http_async_remove(handle, HTTP_ASYNC_SYSERR, GINT_TO_POINTER(code));
}

/**
 * Cancel request (header parsing error).
 */
static void
http_async_headerr(gpointer handle, gint code)
{
	http_async_remove(handle, HTTP_ASYNC_HEADER, GINT_TO_POINTER(code));
}

/**
 * Cancel request (HTTP error).
 */
static void
http_async_http_error(
	gpointer handle, struct header *header, gint code, const gchar *message)
{
	http_error_t he;

	he.header = header;
	he.code = code;
	he.message = message;

	http_async_remove(handle, HTTP_ASYNC_HTTP, &he);
}

/**
 * Default callback invoked to build the HTTP request.
 *
 * The request is to be built in `buf', which is `len' bytes long.
 * The HTTP request is defined by the `verb' ("GET", "HEAD", ...), the
 * `path' to ask for and the `host' to which we are making the request, for
 * suitable "Host:" header emission.
 *
 * @return the length of the generated request, which must be terminated
 * properly by a trailing "\r\n" on a line by itself to mark the end of the
 * header.
 */
static size_t
http_async_build_request(gpointer unused_handle, gchar *buf, size_t len,
	const gchar *verb, const gchar *path, const gchar *host, guint16 port)
{
	size_t rw;
	gchar port_str[32];

	(void) unused_handle;
	g_assert(len <= INT_MAX);

	if (port != HTTP_PORT) {
		gm_snprintf(port_str, sizeof port_str, ":%u", (guint) port);
	} else {
		port_str[0] = '\0';
	}
	rw = gm_snprintf(buf, len,
		"%s %s HTTP/1.1\r\n"
		"Host: %s%s\r\n"
		"User-Agent: %s\r\n"
		"Connection: close\r\n"
		"\r\n",
		verb, path, host, port_str, version_string);

	return rw;
}

/**
 * Internal creation routine for HTTP asynchronous requests.
 *
 * The URL to request is given by `url'.
 * The type of HTTP request (GET, POST, ...) is given by `type'.
 *
 * If `addr' is non-zero, then `url' is supposed to hold a path, and `port'
 * must also be non-zero.  Otherwise, the IP and port are gathered from
 * `url', which must start be something like "http://server:port/path".
 *
 * When all headers are read, optionally call `header_ind' if not-NULL.
 * When data is present, optionally call `data_ind' or close the connection.
 * On error condition during the asynchronous processing, call `error_ind',
 * including when the request is explicitly cancelled (but NOT when it is
 * excplicitly closed).
 *
 * If `parent' is not NULL, then this request is a child request.
 *
 * @return the newly created request, or NULL with `http_async_errno' set.
 */
static struct http_async *
http_async_create(
	gchar *url,						/* Either full URL or path */
	const host_addr_t addr,			/* Optional: 0 means grab from url */
	guint16 port,					/* Optional, must be given when IP given */
	enum http_reqtype type,
	http_header_cb_t header_ind,
	http_data_cb_t data_ind,
	http_error_cb_t error_ind,
	struct http_async *parent)
{
	struct gnutella_socket *s;
	struct http_async *ha;
	const gchar *path, *host = NULL;

	g_assert(url);
	g_assert(error_ind);
	g_assert(!is_host_addr(addr) || port != 0);

	/*
	 * Extract the necessary parameters for the connection.
	 *
	 * When connection is established, http_async_connected() will be called
	 * from the socket layer.
	 */

	if (!is_host_addr(addr)) {
		guint16 uport;

		if (!http_url_parse(url, &uport, &host, &path)) {
			http_async_errno = HTTP_ASYNC_BAD_URL;
			return NULL;
		}

		g_assert(host != NULL);

		s = socket_connect_by_name(host, uport, SOCK_TYPE_HTTP, 0);
	} else {
		host = host_addr_port_to_string(addr, port);
		path = url;

		s = socket_connect(addr, port, SOCK_TYPE_HTTP, 0);
	}

	if (s == NULL) {
		http_async_errno = HTTP_ASYNC_CONN_FAILED;
		return NULL;
	}

	/*
	 * Connection started, build handle and return.
	 */

	ha = walloc(sizeof(*ha));

	s->resource.handle = ha;

	ha->magic = HTTP_ASYNC_MAGIC;
	ha->type = type;
	ha->state = HTTP_AS_CONNECTING;
	ha->flags = 0;
	ha->url = atom_str_get(url);
	ha->path = atom_str_get(path);
	ha->host = atom_str_get(host);
	ha->socket = s;
	ha->header_ind = header_ind;
	ha->data_ind = data_ind;
	ha->error_ind = error_ind;
	ha->state_chg = NULL;
	ha->io_opaque = NULL;
	ha->bio = NULL;
	ha->last_update = tm_time();
	ha->user_opaque = NULL;
	ha->user_free = NULL;
	ha->parent = parent;
	ha->children = NULL;
	ha->delayed = NULL;
	ha->allow_redirects = FALSE;
	ha->op_request = http_async_build_request;

	sl_outgoing = g_slist_prepend(sl_outgoing, ha);

	/*
	 * If request has a parent, insert in parent's children list.
	 */

	if (parent)
		parent->children = g_slist_prepend(parent->children, ha);

	return ha;
}

/**
 * Change the request state, and notify listener if any.
 */
static void
http_async_newstate(struct http_async *ha, http_state_t state)
{
	ha->state = state;
	ha->last_update = tm_time();

	if (ha->state_chg != NULL)
		(*ha->state_chg)(ha, state);
}

/**
 * Starts an asynchronous HTTP GET request on the specified path.
 *
 * @returns a handle on the request if OK, NULL on error with the
 * http_async_errno variable set before returning.
 *
 * When data is available, `data_ind' will be called.  When all data have been
 * read, a final call to `data_ind' is made with no data.  If there is no
 * `data_ind' routine, the connection will be closed after reading the
 * whole header.
 *
 * On error, `error_ind' will be called, and upon return, the request will
 * be automatically cancelled.
 */
gpointer
http_async_get(
	gchar *url,
	http_header_cb_t header_ind,
	http_data_cb_t data_ind,
	http_error_cb_t error_ind)
{
	return http_async_create(url, zero_host_addr, 0, HTTP_GET,
				header_ind, data_ind, error_ind, NULL);
}

/**
 * Same as http_async_get(), but a path on the server is given and the
 * IP and port to contact are given explicitly.
 */
gpointer
http_async_get_addr(
	gchar *path,
	const host_addr_t addr,
	guint16 port,
	http_header_cb_t header_ind,
	http_data_cb_t data_ind,
	http_error_cb_t error_ind)
{
	return http_async_create(path, addr, port, HTTP_GET,
		header_ind, data_ind, error_ind,
		NULL);
}

/**
 * Redefines the building of the HTTP request.
 */
void
http_async_set_op_request(gpointer handle, http_op_request_t op)
{
	struct http_async *ha = (struct http_async *) handle;

	g_assert(ha->magic == HTTP_ASYNC_MAGIC);
	g_assert(op != NULL);

	ha->op_request = op;
}

/**
 * Defines callback to invoke when the request changes states.
 */
void
http_async_on_state_change(gpointer handle, http_state_change_t fn)
{
	struct http_async *ha = (struct http_async *) handle;

	g_assert(ha->magic == HTTP_ASYNC_MAGIC);
	g_assert(fn != NULL);

	ha->state_chg = fn;
}

/**
 * Whether we should follow HTTP redirections (FALSE by default).
 */
void
http_async_allow_redirects(gpointer handle, gboolean allow)
{
	struct http_async *ha = (struct http_async *) handle;

	g_assert(ha->magic == HTTP_ASYNC_MAGIC);

	ha->allow_redirects = allow;
}

/**
 * Interceptor callback for `header_ind' in child requests.
 * Reroute to parent request.
 */
static gboolean
http_subreq_header_ind(
	gpointer handle, struct header *header, gint code, const gchar *message)
{
	struct http_async *ha = (struct http_async *) handle;

	g_assert(ha->magic == HTTP_ASYNC_MAGIC);
	g_assert(ha->parent != NULL);
	g_assert(ha->parent->header_ind);

	return (*ha->parent->header_ind)(ha->parent, header, code, message);
}

/**
 * Interceptor callback for `data_ind' in child requests.
 * Reroute to parent request.
 */
static void
http_subreq_data_ind(
	gpointer handle, gchar *data, gint len)
{
	struct http_async *ha = (struct http_async *) handle;

	g_assert(ha->magic == HTTP_ASYNC_MAGIC);
	g_assert(ha->parent != NULL);
	g_assert(ha->parent->data_ind);

	(*ha->parent->data_ind)(ha->parent, data, len);
}

/**
 * Interceptor callback for `error_ind' in child requests.
 * Reroute to parent request.
 */
static void
http_subreq_error_ind(
	gpointer handle, http_errtype_t error, gpointer val)
{
	struct http_async *ha = (struct http_async *) handle;

	g_assert(ha->magic == HTTP_ASYNC_MAGIC);
	g_assert(ha->parent != NULL);
	g_assert(ha->parent->error_ind);

	(*ha->parent->error_ind)(ha->parent, error, val);
}

/**
 * Create a child request, to follow redirection transparently.
 *
 * All callbacks will be rerouted to the parent request, as if they came
 * from the original parent.
 *
 * @returns whether we succeeded in creating the subrequest.
 */
static gboolean
http_async_subrequest(
	struct http_async *parent, gchar *url, enum http_reqtype type)
{
	struct http_async *child;

	/*
	 * We're installing our own callbacks to transparently reroute them
	 * to the user-supplied callbacks for the parent request, hence making
	 * the sub-request invisible from the outside.
	 */

	child = http_async_create(url, zero_host_addr, 0, type,
		parent->header_ind ? http_subreq_header_ind : NULL,	/* Optional */
		parent->data_ind ? http_subreq_data_ind : NULL,		/* Optional */
		http_subreq_error_ind,
		parent);

	/*
	 * Propagate any redefined operation.
	 */

	child->op_request = parent->op_request;

	/*
	 * Indicate that the child request now has control, the parent request
	 * being only there to record the user's callbacks (and because it's the
	 * only one known from the outside).
	 */

	if (child)
		parent->flags |= HA_F_SUBREQ;

	return child != NULL;
}

/**
 * Redirect current HTTP request to some other URL.
 */
static void
http_redirect(struct http_async *ha, gchar *url)
{
	/*
	 * If this request already has a parent, then we're already
	 * a redirection.  We're currently not allowing it.  When we do, it
	 * will have to be limited anyway to avoid endless redirections.
	 */

	if (ha->parent) {
		http_async_error(ha, HTTP_ASYNC_NESTED);
		return;
	}

	/*
	 * Close connection of parent request.
	 *
	 */

	g_assert(ha->socket);

	socket_free_null(&ha->socket);
	http_async_newstate(ha, HTTP_AS_REDIRECTED);

	/*
	 * Create sub-request to handle the redirection.
	 */

	if (!http_async_subrequest(ha, url, ha->type)) {
		http_async_error(ha, http_async_errno);
		return;
	}

	/*
	 * Free useless I/O opaque structure (a new one will be created when the
	 * subrequest has its socket connected).
	 *
	 * NB: We have to do this after http_async_subrequest(), since `url' is
	 * actually pointing inside the header data in the io_opaque structure: it
	 * is extracted from the Location: header.
	 */

	g_assert(ha->io_opaque);
	g_assert(ha->bio == NULL);		/* Have not started to read data */

	io_free(ha->io_opaque);
	ha->io_opaque = NULL;
}

/**
 * Tell the user that we got new data for his request.
 * If `eof' is TRUE, this is the last data we'll get.
 */
static void
http_got_data(struct http_async *ha, gboolean eof)
{
	struct gnutella_socket *s = ha->socket;

	g_assert(s);
	g_assert(eof || s->pos > 0);		/* If not EOF, there must be data */

	if (s->pos > 0) {
		ha->last_update = tm_time();
		(*ha->data_ind)(ha, s->buffer, s->pos);
		if (ha->flags & HA_F_FREED)		/* Callback decided to cancel/close */
			return;
		s->pos = 0;
	}

	if (eof) {
		(*ha->data_ind)(ha, NULL, 0);	/* Indicates EOF */
		if (ha->flags & HA_F_FREED)		/* Callback decided to cancel/close */
			return;
		http_async_free(ha);
	}
}

/**
 * Called when data are available on the socket.
 * Read them and pass them to http_got_data().
 */
static void
http_data_read(gpointer data, gint unused_source, inputevt_cond_t cond)
{
	struct http_async *ha = (struct http_async *) data;
	struct gnutella_socket *s = ha->socket;
	ssize_t r;

	(void) unused_source;
	g_assert(ha->magic == HTTP_ASYNC_MAGIC);

	if (cond & INPUT_EVENT_EXCEPTION) {
		socket_eof(s);
		http_async_error(ha, HTTP_ASYNC_IO_ERROR);
		return;
	}

	g_assert((gint) s->pos >= 0 && s->pos <= sizeof s->buffer);

	if (s->pos == sizeof(s->buffer)) {
		http_async_error(ha, HTTP_ASYNC_IO_ERROR);
		return;
	}

	r = bio_read(ha->bio, s->buffer + s->pos, sizeof(s->buffer) - s->pos);
	if (r == 0) {
		socket_eof(s);
		http_got_data(ha, TRUE);			/* Signals EOF */
		return;
	} else if ((ssize_t) -1 == r) {
		if (!is_temporary_error(errno)) {
			socket_eof(s);
			http_async_syserr(ha, errno);
		}
		return;
	}

	s->pos += r;

	http_got_data(ha, FALSE);				/* EOF not reached yet */
}

/**
 * Called when the whole server's reply header was parsed.
 */
static void
http_got_header(struct http_async *ha, header_t *header)
{
	struct gnutella_socket *s = ha->socket;
	const gchar *status = getline_str(s->getline);
	gint ack_code;
	const gchar *ack_message = "";
	gchar *buf;
	guint http_major = 0, http_minor = 0;

	if (http_debug > 2) {
		printf("----Got HTTP reply from %s:\n", host_addr_to_string(s->addr));
		printf("%s\n", status);
		header_dump(header, stdout);
		printf("----\n");
		fflush(stdout);
	}

	/*
	 * Check status.
	 */

	ack_code = http_status_parse(status, "HTTP",
		&ack_message, &http_major, &http_minor);

	if (ack_code == -1) {
		http_async_error(ha, HTTP_ASYNC_BAD_STATUS);
		return;
	}

	/*
	 * Notify them that we got the headers.
	 * Don't continue if the callback returns FALSE.
	 */

	if (
		ha->header_ind &&
		!(*ha->header_ind)(ha, header, ack_code, ack_message)
	)
		return;

	/*
	 * Deal with return code.
	 */

	switch (ack_code) {
	case 200:					/* OK */
		break;
	case 301:					/* Moved permanently */
	case 302:					/* Found */
	case 303:					/* See other */
	case 307:					/* Moved temporarily */
		if (!ha->allow_redirects) {
			http_async_error(ha, HTTP_ASYNC_REDIRECTED);
			return;
		}

		buf = header_get(header, "Location");
		if (buf == NULL) {
			http_async_error(ha, HTTP_ASYNC_NO_LOCATION);
			return;
		}

		/*
		 * On 302, we can only blindly follow the redirection if the original
		 * request was a GET or a HEAD.
		 */

		if (
			ack_code != 302 ||
			(ack_code == 302 && (ha->type == HTTP_GET || ha->type == HTTP_HEAD))
		) {
			if (http_debug > 2)
				printf("HTTP %s redirect %d (%s): \"%s\" -> \"%s\"\n",
					http_verb[ha->type], ack_code, ack_message, ha->url, buf);

			/*
			 * The Location: header MUST be an absolute URI, according to
			 * RFC-2616 (HTTP/1.1 specs).
			 */

			if (!http_url_parse(buf, NULL, NULL, NULL)) {
				http_async_error(ha, HTTP_ASYNC_BAD_LOCATION_URI);
				return;
			}

			http_redirect(ha, buf);
			return;
		}
		/* FALL THROUGH */
	default:					/* Error */
		http_async_http_error(ha, header, ack_code, ack_message);
		return;
	}

	/*
	 * If there is no callback for data reception, we're done.
	 */

	if (ha->data_ind == NULL) {
		http_async_error(ha, HTTP_ASYNC_CLOSED);
		return;
	}

	/*
	 * Prepare reception of data.
	 */

	g_assert(s->gdk_tag == 0);
	g_assert(ha->bio == NULL);

	ha->bio = bsched_source_add(bws.in, &s->wio,
		BIO_F_READ, http_data_read, (gpointer) ha);

	/*
	 * We may have something left in the input buffer.
	 * Give them the data immediately.
	 */

	http_async_newstate(ha, HTTP_AS_RECEIVING);

	if (s->pos > 0)
		http_got_data(ha, FALSE);
}

/**
 * Get the state of the HTTP request.
 */
http_state_t
http_async_state(gpointer handle)
{
	struct http_async *ha = (struct http_async *) handle;

	g_assert(ha->magic == HTTP_ASYNC_MAGIC);

	/*
	 * Special-case redirected request: they have at least one child.
	 * @return the state of the first active child we get.
	 */

	if (ha->state == HTTP_AS_REDIRECTED) {
		GSList *l;

		g_assert(ha->children);

		for (l = ha->children; l; l = g_slist_next(l)) {
			struct http_async *cha = (struct http_async *) l->data;

			switch (cha->state) {
			case HTTP_AS_REDIRECTED:
			case HTTP_AS_REMOVED:
				break;
			default:
				return cha->state;
			}
		}

		return HTTP_AS_UNKNOWN;		/* Weird */
	}

	return ha->state;
}

/**
 ** HTTP header parsing dispatching callbacks.
 **/

/**
 * Called when full header was collected.
 */
static void
call_http_got_header(gpointer obj, header_t *header)
{
	struct http_async *ha = (struct http_async *) obj;

	g_assert(ha->magic == HTTP_ASYNC_MAGIC);

	http_got_header(ha, header);
}

static struct io_error http_io_error;

/**
 * Called when we start receiving the HTTP headers.
 */
static void
http_header_start(gpointer handle)
{
	struct http_async *ha = (struct http_async *) handle;

	g_assert(ha->magic == HTTP_ASYNC_MAGIC);

	http_async_newstate(ha, HTTP_AS_HEADERS);
}

/**
 * Called when the whole HTTP request has been sent out.
 */
static void
http_async_request_sent(struct http_async *ha)
{
	http_async_newstate(ha, HTTP_AS_REQ_SENT);

	/*
	 * Prepare to read back the status line and the headers.
	 */

	g_assert(ha->io_opaque == NULL);

	io_get_header(ha, &ha->io_opaque, bws.in, ha->socket, IO_SAVE_FIRST,
		call_http_got_header, http_header_start, &http_io_error);
}

/**
 * I/O callback invoked when we can write more data to the server to finish
 * sending the HTTP request.
 */
static void
http_async_write_request(gpointer data, gint unused_source,
	inputevt_cond_t cond)
{
	struct http_async *ha = (struct http_async *) data;
	struct gnutella_socket *s = ha->socket;
	http_buffer_t *r = ha->delayed;
	ssize_t sent;
	size_t rw;
	gchar *base;

	(void) unused_source;
	g_assert(ha->magic == HTTP_ASYNC_MAGIC);
	g_assert(r != NULL);
	g_assert(ha->state == HTTP_AS_REQ_SENDING);

	if (cond & INPUT_EVENT_EXCEPTION) {
		socket_eof(s);
		http_async_error(ha, HTTP_ASYNC_IO_ERROR);
		return;
	}

	rw = http_buffer_unread(r);			/* Data we still have to send */
	base = http_buffer_read_base(r);	/* And where unsent data start */

	sent = bws_write(bws.out, &s->wio, base, rw);
	if ((ssize_t) -1 == sent) {
		g_warning("HTTP request sending to %s failed: %s",
			host_addr_port_to_string(s->addr, s->port), g_strerror(errno));
		http_async_syserr(ha, errno);
		return;
	} else if ((size_t) sent < rw) {
		http_buffer_add_read(r, sent);
		return;
	} else if (http_debug > 2) {
		printf("----Sent HTTP request completely to %s (%d bytes):\n%.*s----\n",
			host_addr_port_to_string(s->addr, s->port), http_buffer_length(r),
			http_buffer_length(r), http_buffer_base(r));
		fflush(stdout);
	}

	/*
	 * HTTP request was completely sent.
	 */

	if (http_debug)
		g_warning("flushed partially written HTTP request to %s (%d bytes)",
			host_addr_port_to_string(s->addr, s->port),
			http_buffer_length(r));

	socket_evt_clear(s);

	http_buffer_free(r);
	ha->delayed = NULL;

	http_async_request_sent(ha);
}

/**
 * Callback from the socket layer when the connection to the remote
 * server is made.
 */
void
http_async_connected(gpointer handle)
{
	struct http_async *ha = (struct http_async *) handle;
	struct gnutella_socket *s = ha->socket;
	size_t rw;
	ssize_t sent;
	gchar req[2048];

	g_assert(ha->magic == HTTP_ASYNC_MAGIC);
	g_assert(s);
	g_assert(s->resource.handle == handle);

	/*
	 * Build the HTTP request.
	 */

	rw = (*ha->op_request)(ha, req, sizeof(req),
		(gchar *) http_verb[ha->type], ha->path,
		ha->host ? ha->host : host_addr_to_string(s->addr), s->port);

	if (rw >= sizeof(req)) {
		http_async_error(ha, HTTP_ASYNC_REQ2BIG);
		return;
	}

	/*
	 * Send the HTTP request.
	 */

	http_async_newstate(ha, HTTP_AS_REQ_SENDING);

	sent = bws_write(bws.out, &s->wio, req, rw);
	if ((ssize_t) -1 == sent) {
		g_warning("HTTP request sending to %s failed: %s",
			host_addr_port_to_string(s->addr, s->port), g_strerror(errno));
		http_async_syserr(ha, errno);
		return;
	} else if ((size_t) sent < rw) {
		g_warning("partial HTTP request write to %s: only %d of %d bytes sent",
			host_addr_port_to_string(s->addr, s->port), (gint) sent, (gint) rw);

		g_assert(ha->delayed == NULL);

		ha->delayed = http_buffer_alloc(req, rw, sent);

		/*
		 * Install the writing callback.
		 */

		g_assert(s->gdk_tag == 0);

		socket_evt_set(s, INPUT_EVENT_WX, http_async_write_request, ha);

		return;
	} else if (http_debug > 2) {
		g_message("----Sent HTTP request to %s (%d bytes):\n%.*s----",
			host_addr_port_to_string(s->addr, s->port), (int) rw, (int) rw, req);
	}

	http_async_request_sent(ha);
}

/**
 * Error indication callback which logs the error by listing the
 * initial HTTP request and the reported error cause.  The specified
 * debugging level is explicitly given.
 */
void
http_async_log_error_dbg(
	gpointer handle, http_errtype_t type, gpointer v, guint32 dbg_level)
{
	const gchar *url;
	const gchar *req;
	gint error = GPOINTER_TO_INT(v);
	http_error_t *herror = (http_error_t *) v;
	host_addr_t addr;
	guint16 port;

	url = http_async_info(handle, &req, NULL, &addr, &port);

	switch (type) {
	case HTTP_ASYNC_SYSERR:
        if (dbg_level) {
            g_message("aborting \"%s %s\" at %s on system error: %s",
                req, url, host_addr_port_to_string(addr, port),
				g_strerror(error));
        }
		return;
	case HTTP_ASYNC_ERROR:
		if (error == HTTP_ASYNC_CANCELLED) {
			if (dbg_level > 3)
				g_message("explicitly cancelled \"%s %s\" at %s", req, url,
					host_addr_port_to_string(addr, port));
		} else if (error == HTTP_ASYNC_CLOSED) {
			if (dbg_level > 3)
				g_message("connection closed for \"%s %s\" at %s", req, url,
					host_addr_port_to_string(addr, port));
		} else
            if (dbg_level) {
                g_message("aborting \"%s %s\" at %s on error: %s", req, url,
                    host_addr_port_to_string(addr, port),
					http_async_strerror(error));
            }
		return;
	case HTTP_ASYNC_HEADER:
        if (dbg_level) {
            g_message("aborting \"%s %s\" at %s on header parsing error: %s",
                req, url, host_addr_port_to_string(addr, port),
				header_strerror(error));
        }
		return;
	case HTTP_ASYNC_HTTP:
        if (dbg_level) {
            g_message("stopping \"%s %s\" at %s: HTTP %d %s", req, url,
                host_addr_port_to_string(addr, port),
				herror->code, herror->message);
        }
		return;
	/* No default clause, let the compiler warn us about missing cases. */
	}

	/* In case the error was not trapped at compile time... */
	g_error("unhandled HTTP request error type %d", type);
	/* NOTREACHED */
}

/**
 * Default error indication callback which logs the error by listing the
 * initial HTTP request and the reported error cause.
 */
void
http_async_log_error(gpointer handle, http_errtype_t type, gpointer v)
{
	return http_async_log_error_dbg(handle, type, v, http_debug);
}

/***
 *** I/O header parsing callbacks.
 ***/

static void
err_line_too_long(gpointer obj)
{
	http_async_error(obj, HTTP_ASYNC_HEAD2BIG);
}

static void
err_header_error(gpointer obj, gint error)
{
	http_async_headerr(obj, error);
}

static void
err_input_exception(gpointer obj)
{
	http_async_error(obj, HTTP_ASYNC_IO_ERROR);
}

static void
err_input_buffer_full(gpointer obj)
{
	http_async_error(obj, HTTP_ASYNC_IO_ERROR);
}

static void
err_header_read_error(gpointer obj, gint error)
{
	http_async_syserr(obj, error);
}

static void
err_header_read_eof(gpointer obj)
{
	http_async_error(obj, HTTP_ASYNC_EOF);
}

static struct io_error http_io_error = {
	err_line_too_long,
	NULL,
	err_header_error,
	err_input_exception,
	err_input_buffer_full,
	err_header_read_error,
	err_header_read_eof,
	NULL,
};

/**
 * Called from main timer to expire HTTP requests that take too long.
 */
void
http_timer(time_t now)
{
	GSList *l;

retry:
	for (l = sl_outgoing; l; l = l->next) {
		struct http_async *ha = (struct http_async *) l->data;
		gint elapsed = delta_time(now, ha->last_update);
		gint timeout = ha->bio ?
			download_connected_timeout :
			download_connecting_timeout;

		if (ha->flags & HA_F_SUBREQ)
			continue;

		if (elapsed > timeout) {
			switch (ha->state) {
			case HTTP_AS_UNKNOWN:
			case HTTP_AS_CONNECTING:
				http_async_error(ha, HTTP_ASYNC_CONN_TIMEOUT);
				goto retry;
			case HTTP_AS_REMOVED:
				g_error("removed async request should not be listed");
				break;
			default:
				http_async_error(ha, HTTP_ASYNC_TIMEOUT);
				goto retry;
			}
		}
	}

	/*
	 * Dispose of the logically freed structures, asynchronously.
	 */

	if (sl_ha_freed)
		http_async_free_pending();
}

/**
 * Shutdown the HTTP module.
 */
void
http_close(void)
{
	while (sl_outgoing)
		http_async_error(
			(struct http_async *) sl_outgoing->data, HTTP_ASYNC_CANCELLED);
}

/* vi: set ts=4 sw=4 cindent: */
