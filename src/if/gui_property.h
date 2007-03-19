/*
 * Copyright (c) 2001-2003, Richard Eckart
 *
 * THIS FILE IS AUTOGENERATED! DO NOT EDIT!
 * This file is generated from gui_props.ag using autogen.
 * Autogen is available at http://autogen.sourceforge.net/.
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

#ifndef _gui_property_h_
#define _gui_property_h_


#include "lib/prop.h"

#define GUI_PROPERTY_MIN (1000)
#define GUI_PROPERTY_MAX (1000+GUI_PROPERTY_END-1)
#define GUI_PROPERTY_NUM (GUI_PROPERTY_END-1000)

typedef enum {
    PROP_MONITOR_ENABLED=1000,
    PROP_MONITOR_MAX_ITEMS,
    PROP_QUEUE_REGEX_CASE,
    PROP_FI_REGEX_CASE,
    PROP_SEARCH_HIDE_DOWNLOADED,
    PROP_NODES_COL_WIDTHS,
    PROP_NODES_COL_VISIBLE,
    PROP_DL_ACTIVE_COL_WIDTHS,
    PROP_DL_ACTIVE_COL_VISIBLE,
    PROP_DL_QUEUED_COL_WIDTHS,
    PROP_DL_QUEUED_COL_VISIBLE,
    PROP_FILE_INFO_COL_WIDTHS,
    PROP_SEARCH_LIST_COL_WIDTHS,
    PROP_SEARCH_RESULTS_COL_VISIBLE,
    PROP_SEARCH_RESULTS_COL_WIDTHS,
    PROP_SEARCH_STATS_COL_WIDTHS,
    PROP_UL_STATS_COL_WIDTHS,
    PROP_UL_STATS_COL_VISIBLE,
    PROP_UPLOADS_COL_WIDTHS,
    PROP_UPLOADS_COL_VISIBLE,
    PROP_FILTER_RULES_COL_WIDTHS,
    PROP_FILTER_FILTERS_COL_WIDTHS,
    PROP_GNET_STATS_MSG_COL_WIDTHS,
    PROP_GNET_STATS_FC_TTL_COL_WIDTHS,
    PROP_GNET_STATS_FC_HOPS_COL_WIDTHS,
    PROP_GNET_STATS_FC_COL_WIDTHS,
    PROP_GNET_STATS_HORIZON_COL_WIDTHS,
    PROP_GNET_STATS_DROP_REASONS_COL_WIDTHS,
    PROP_GNET_STATS_RECV_COL_WIDTHS,
    PROP_HCACHE_COL_WIDTHS,
    PROP_WINDOW_COORDS,
    PROP_FILTER_DLG_COORDS,
    PROP_PREFS_DLG_COORDS,
    PROP_FILEINFO_DIVIDER_POS,
    PROP_MAIN_DIVIDER_POS,
    PROP_GNET_STATS_DIVIDER_POS,
    PROP_SIDE_DIVIDER_POS,
    PROP_RESULTS_DIVIDER_POS,
    PROP_SEARCH_MAX_RESULTS,
    PROP_BROWSE_HOST_MAX_RESULTS,
    PROP_GUI_DEBUG,
    PROP_FILTER_MAIN_DIVIDER_POS,
    PROP_SEARCH_RESULTS_SHOW_TABS,
    PROP_SEARCHBAR_VISIBLE,
    PROP_SIDEBAR_VISIBLE,
    PROP_NAVTREE_VISIBLE,
    PROP_TOOLBAR_VISIBLE,
    PROP_STATUSBAR_VISIBLE,
    PROP_PROGRESSBAR_UPLOADS_VISIBLE,
    PROP_PROGRESSBAR_DOWNLOADS_VISIBLE,
    PROP_PROGRESSBAR_CONNECTIONS_VISIBLE,
    PROP_PROGRESSBAR_BWS_IN_VISIBLE,
    PROP_PROGRESSBAR_BWS_OUT_VISIBLE,
    PROP_PROGRESSBAR_BWS_GIN_VISIBLE,
    PROP_PROGRESSBAR_BWS_GOUT_VISIBLE,
    PROP_PROGRESSBAR_BWS_GLIN_VISIBLE,
    PROP_PROGRESSBAR_BWS_GLOUT_VISIBLE,
    PROP_AUTOHIDE_BWS_GLEAF,
    PROP_PROGRESSBAR_BWS_IN_AVG,
    PROP_PROGRESSBAR_BWS_OUT_AVG,
    PROP_PROGRESSBAR_BWS_GIN_AVG,
    PROP_PROGRESSBAR_BWS_GOUT_AVG,
    PROP_PROGRESSBAR_BWS_GLIN_AVG,
    PROP_PROGRESSBAR_BWS_GLOUT_AVG,
    PROP_SEARCH_SORT_CASESENSE,
    PROP_SEARCH_SORT_DEFAULT_ORDER,
    PROP_SEARCH_SORT_DEFAULT_COLUMN,
    PROP_SEARCH_DISCARD_SPAM,
    PROP_SEARCH_DISCARD_HASHLESS,
    PROP_SEARCH_JUMP_TO_CREATED,
    PROP_SHOW_DL_SETTINGS,
    PROP_SEARCH_STATS_MODE,
    PROP_SEARCH_STATS_UPDATE_INTERVAL,
    PROP_SEARCH_STATS_DELCOEF,
    PROP_CONFIRM_QUIT,
    PROP_SHOW_TOOLTIPS,
    PROP_EXPERT_MODE,
    PROP_GNET_STATS_PERC,
    PROP_GNET_STATS_BYTES,
    PROP_GNET_STATS_HOPS,
    PROP_GNET_STATS_SOURCE,
    PROP_GNET_STATS_DROP_REASONS_TYPE,
    PROP_GNET_STATS_WITH_HEADERS,
    PROP_GNET_STATS_DROP_PERC,
    PROP_GNET_STATS_GENERAL_COL_WIDTHS,
    PROP_AUTOCLEAR_COMPLETED_UPLOADS,
    PROP_AUTOCLEAR_FAILED_UPLOADS,
    PROP_NODE_SHOW_UPTIME,
    PROP_NODE_SHOW_HANDSHAKE_VERSION,
    PROP_NODE_SHOW_DETAILED_INFO,
    PROP_SHOW_GNET_INFO_TXC,
    PROP_SHOW_GNET_INFO_RXC,
    PROP_SHOW_GNET_INFO_TX_WIRE,
    PROP_SHOW_GNET_INFO_RX_WIRE,
    PROP_SHOW_GNET_INFO_TX_SPEED,
    PROP_SHOW_GNET_INFO_RX_SPEED,
    PROP_SHOW_GNET_INFO_TX_QUERIES,
    PROP_SHOW_GNET_INFO_RX_QUERIES,
    PROP_SHOW_GNET_INFO_TX_HITS,
    PROP_SHOW_GNET_INFO_RX_HITS,
    PROP_SHOW_GNET_INFO_GEN_QUERIES,
    PROP_SHOW_GNET_INFO_SQ_QUERIES,
    PROP_SHOW_GNET_INFO_TX_DROPPED,
    PROP_SHOW_GNET_INFO_RX_DROPPED,
    PROP_SHOW_GNET_INFO_QRP_STATS,
    PROP_SHOW_GNET_INFO_DBW,
    PROP_SHOW_GNET_INFO_RT,
    PROP_SHOW_GNET_INFO_SHARED_SIZE,
    PROP_SHOW_GNET_INFO_SHARED_FILES,
    PROP_SEARCH_ACCUMULATION_PERIOD,
    PROP_TREEMENU_NODES_EXPANDED,
    PROP_GNET_STATS_PKG_COL_WIDTHS,
    PROP_GNET_STATS_BYTE_COL_WIDTHS,
    PROP_CONFIG_TOOLBAR_STYLE,
    PROP_SEARCH_LIFETIME,
    GUI_PROPERTY_END
} gui_property_t;

/*
 * Property set stub
 */
prop_set_stub_t *gui_prop_get_stub(void);

/*
 * Property definition
 */
prop_def_t *gui_prop_get_def(property_t);
property_t gui_prop_get_by_name(const gchar *);
GSList *gui_prop_get_by_regex(const gchar *, gint *);
const gchar *gui_prop_name(property_t);
const gchar *gui_prop_type_to_string(property_t);
const gchar *gui_prop_to_string(property_t prop);
const gchar *gui_prop_default_to_string(property_t);
const gchar *gui_prop_description(property_t);
gboolean gui_prop_is_saved(property_t);
void gui_prop_set_from_string(property_t, const gchar *);

/*
 * Property-change listeners
 */
void gui_prop_add_prop_changed_listener(
    property_t, prop_changed_listener_t, gboolean);
void gui_prop_remove_prop_changed_listener(
    property_t, prop_changed_listener_t);

/*
 * get/set functions
 *
 * The *_val macros are shortcuts for single scalar properties.
 */
void gui_prop_set_boolean(
    property_t, const gboolean *, size_t, size_t);
gboolean *gui_prop_get_boolean(
    property_t, gboolean *, size_t, size_t);

#define gui_prop_set_boolean_val(p, v) G_STMT_START { \
	gboolean value = v; \
	gui_prop_set_boolean(p, &value, 0, 1); \
} G_STMT_END

#define gui_prop_get_boolean_val(p, v) G_STMT_START { \
	gui_prop_get_boolean(p, v, 0, 1); \
} G_STMT_END


void gui_prop_set_string(property_t, const gchar *);
gchar *gui_prop_get_string(property_t, gchar *, size_t);

void gui_prop_set_guint32(
    property_t, const guint32 *, size_t, size_t);
guint32 *gui_prop_get_guint32(
    property_t, guint32 *, size_t, size_t);

#define gui_prop_set_guint32_val(p, v) G_STMT_START { \
	guint32 value = v; \
	gui_prop_set_guint32(p, &value, 0, 1); \
} G_STMT_END

#define gui_prop_get_guint32_val(p, v) G_STMT_START { \
	gui_prop_get_guint32(p, v, 0, 1); \
} G_STMT_END

void gui_prop_set_guint64(
    property_t, const guint64 *, size_t, size_t);
guint64 *gui_prop_get_guint64(
    property_t, guint64 *, size_t, size_t);

#define gui_prop_set_guint64_val(p, v) G_STMT_START { \
	guint64 value = v; \
	gui_prop_set_guint64(p, &value, 0, 1); \
} G_STMT_END

#define gui_prop_get_guint64_val(p, v) G_STMT_START { \
	gui_prop_get_guint64(p, v, 0, 1); \
} G_STMT_END

void gui_prop_set_timestamp(
    property_t, const time_t *, size_t, size_t);
time_t *gui_prop_get_timestamp(
    property_t, time_t *, size_t, size_t);

#define gui_prop_set_timestamp_val(p, v) G_STMT_START { \
	time_t value = v; \
	gui_prop_set_timestamp(p, &value, 0, 1); \
} G_STMT_END

#define gui_prop_get_timestamp_val(p, v) G_STMT_START { \
	gui_prop_get_timestamp(p, v, 0, 1); \
} G_STMT_END

void gui_prop_set_ip(
    property_t, const host_addr_t *, size_t, size_t);
host_addr_t *gui_prop_get_ip(
    property_t, host_addr_t *, size_t, size_t);

#define gui_prop_set_ip_val(p, v) G_STMT_START { \
	host_addr_t value = v; \
	gui_prop_set_ip(p, &value, 0, 1); \
} G_STMT_END

#define gui_prop_get_ip_val(p, v) G_STMT_START { \
	gui_prop_get_ip(p, v, 0, 1); \
} G_STMT_END

void gui_prop_set_storage(property_t, gconstpointer, size_t);
gpointer gui_prop_get_storage(property_t, gpointer, size_t);

#endif /* _gui_property_h_ */

