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
 * @file
 *
 * Globally Unique ID (GUID) manager.
 *
 * HEC generation code is courtesy of Charles Michael Heard (initially
 * written for ATM, but adapted for GTKG, with leading coset leader
 * changed).
 */

#include "common.h"

RCSID("$Id$");

#include "guid.h"
#include "lib/misc.h"
#include "lib/endian.h"

#include "if/gnet_property_priv.h"

#include "lib/override.h"		/* Must be the last header included */

/*
 * Flags for GUID[15] tagging.
 */

#define GUID_PONG_CACHING	0x01
#define GUID_PERSISTENT		0x02

/*
 * Flags for GUID[15] query tagging.
 */

#define GUID_REQUERY		0x01	/* Cleared means initial query */

/*
 * HEC constants.
 */

#define HEC_GENERATOR	0x107		/* x^8 + x^2 + x + 1 */
#define HEC_GTKG_MASK	0x0c3		/* HEC GTKG's mask */

gchar blank_guid[16] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };

static guint8 syndrome_table[256];
static guint16 gtkg_version_mark;

/**
 * Generate a table of CRC-8 syndromes for all possible input bytes.
 */
static void
guid_gen_syndrome_table(void)
{
	guint i;
	guint j;

	for (i = 0; i < 256; i++) {
		guint syn = i;
		for (j = 0; j < 8; j++) {
			syn <<= 1;
			if (syn & 0x80)
				syn ^= HEC_GENERATOR;
		}
		syndrome_table[i] = (guint8) syn;
	}
}

/**
 * Encode major/minor version into 16 bits.
 * If `rel' is true, we're a release, otherwise we're unstable or a beta.
 */
static guint16
guid_gtkg_encode_version(guint major, guint minor, gboolean rel)
{
	guint8 low;
	guint8 high;

	g_assert(major < 0x10);
	g_assert(minor < 0x80);

	/*
	 * Low byte of result is the minor number.
	 * MSB is set for unstable releases.
	 */

	low = minor;

	if (!rel)
		low |= 0x80;

	/*
	 * High byte is divided into two:
	 * . the lowest quartet is the major number.
	 * . the highest quartet is a combination of major/minor.
	 */

	high = (major & 0x0f) | \
		(0xf0 & ((minor << 4) ^ (minor & 0xf0) ^ (major << 4)));

	return (high << 8) | low;
}

/**
 * Compute GUID's HEC over bytes 1..15
 */
static guint8
guid_hec(const gchar *xuid)
{
	gint i;
	guint8 hec = 0;

	for (i = 1; i < 16; i++)
		hec = syndrome_table[hec ^ (guchar) xuid[i]];

	return hec ^ HEC_GTKG_MASK;
}

/**
 * Initialize GUID management.
 */
void
guid_init(void)
{
	gchar *rev = GTA_REVCHAR;		/* Empty string means stable release */

	guid_gen_syndrome_table();

	gtkg_version_mark =
		guid_gtkg_encode_version(GTA_VERSION, GTA_SUBVERSION, *rev == '\0');

	if (dbg)
		printf("GTKG version mark is 0x%x\n", gtkg_version_mark);
}

/**
 * Make sure the MUID we use in initial handshaking pings are marked
 * specially to indicate we're modern nodes.
 */
static void
guid_flag_modern(gchar *muid)
{
	/*
	 * We're a "modern" client, meaning we're not Gnutella 0.56.
	 * Therefore we must set our ninth byte, muid[8] to 0xff, and
	 * put the protocol version number in muid[15].	For 0.4, this
	 * means 0.
	 *				--RAM, 15/09/2001
	 */

	muid[8] = 0xff;
	muid[15] = GUID_PONG_CACHING | GUID_PERSISTENT;
}

/**
 * Flag a GUID/MUID as being from GTKG, by patching `xuid' in place.
 *
 * Bytes 2/3 become the GTKG version mark.
 * Byte 0 becomes the HEC of the remaining 15 bytes.
 */
static void
guid_flag_gtkg(gchar *xuid)
{
	xuid[2] = gtkg_version_mark >> 8;
	xuid[3] = gtkg_version_mark & 0xff;
	xuid[0] = guid_hec(xuid);
}

/**
 * Test whether GUID is that of GTKG, and extract version major/minor, along
 * with release status provided the `majp', `minp' and `relp' are non-NULL.
 */
gboolean
guid_is_gtkg(const gchar *guid, guint8 *majp, guint8 *minp, gboolean *relp)
{
	guint8 major;
	guint8 minor;
	gboolean release;
	guint16 mark;
	guint16 xmark;
	const guint8 *xuid = (const guint8 *) guid;

	if (xuid[0] != guid_hec(guid))
		return FALSE;

	major = xuid[2] & 0x0f;
	minor = xuid[3] & 0x7f;
	release = (xuid[3] & 0x80) ? FALSE : TRUE;

	mark = guid_gtkg_encode_version(major, minor, release);
	xmark = (xuid[2] << 8) | xuid[3];

	if (mark != xmark)
		return FALSE;

	/*
	 * We've validated the GUID: the HEC is correct and the version is
	 * consistently encoded, judging by the highest 4 bits of xuid[2].
	 */

	if (majp) *majp = major;
	if (minp) *minp = minor;
	if (relp) *relp = release;

	return TRUE;
}

/**
 * Test whether a GTKG MUID in a Query is marked as being a retry.
 */
gboolean
guid_is_requery(const gchar *xuid)
{
	return (xuid[15] & GUID_REQUERY) ? TRUE : FALSE;
}

/**
 * Generate a new random GUID, flagged as GTKG.
 */
void
guid_random_muid(gchar *muid)
{
	guid_random_fill(muid);
	guid_flag_gtkg(muid);		/* Mark as being from GTKG */
}

/**
 * Generate a new random (modern) message ID for pings.
 */
void
guid_ping_muid(gchar *muid)
{
	guid_random_fill(muid);
	guid_flag_modern(muid);
	guid_flag_gtkg(muid);		/* Mark as being from GTKG */
}

/**
 * Generate a new random message ID for queries.
 * If `initial' is false, this is a requery.
 */
void
guid_query_muid(gchar *muid, gboolean initial)
{
	guid_random_fill(muid);

	if (initial)
		muid[15] &= ~GUID_REQUERY;
	else
		muid[15] |= GUID_REQUERY;

	guid_flag_gtkg(muid);		/* Mark as being from GTKG */
}

/**
 * Check whether the MUID of a query is that of GTKG.
 *
 * GTKG uses GUID tagging, but unfortunately, the bytes uses to store the
 * IP and port for OOB query hit delivery conflict with the bytes used for
 * the tagging.  Hence the need for a special routine.
 *
 * @param guid	the MUID of the message
 * @param oob	whether the query requests OOB query hit delivery
 * @param majp	where the major release version is written, if GTKG
 * @param minp	where the minor release version is written, if GTKG
 * @param relp	where the release indicator gets written, if GTKG
 */
gboolean
guid_query_muid_is_gtkg(
	const gchar *guid, gboolean oob, guint8 *majp, guint8 *minp, gboolean *relp)
{
	// XXX change when GTKG generates OOB queries
	if (oob)
		return FALSE;

	return guid_is_gtkg(guid, majp, minp, relp);	/* Plain old markup */
}

/**
 * Extract the IP and port number from the GUID of queries marked for OOB
 * query hit delivery.
 *
 * Bytes 0 to 3 of the guid are the 4 octet bytes of the IP address.
 * Bytes 13 and 14 are the little endian representation of the port.
 */
void
guid_oob_get_ip_port(const gchar *guid, guint32 *ip, guint16 *port)
{
	if (ip) {
		guint32 i;
		READ_GUINT32_BE(&guid[0], i);
		*ip = i;
	}
	if (port) {
		guint16 p;
		READ_GUINT16_LE(&guid[13], p);
		*port = p;
	}
}

/* vi: set ts=4: */
