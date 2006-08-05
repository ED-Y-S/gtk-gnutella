/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Richard Eckart
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
 * @ingroup gtk
 * @file
 *
 * Needs short description here.
 *
 * @author Richard Eckart
 * @date 2001-2003
 */

#include "gtk/gui.h"

#include "interface-glade.h"
#if !GTK_CHECK_VERSION(2,5,0)
#include "pbarcellrenderer.h"
#endif

#include "gtk/uploads.h"
#include "gtk/uploads_common.h"
#include "gtk/columns.h"
#include "gtk/gtk-missing.h"
#include "gtk/misc.h"
#include "gtk/notebooks.h"
#include "gtk/settings.h"

#include "if/gui_property.h"
#include "if/bridge/ui2c.h"

#include "lib/atoms.h"
#include "lib/host_addr.h"
#include "lib/glib-missing.h"
#include "lib/iso3166.h"
#include "lib/misc.h"
#include "lib/tm.h"
#include "lib/walloc.h"
#include "lib/override.h"		/* Must be the last header included */

RCSID("$Id$")

#define UPDATE_MIN	300		/**< Update screen every 5 minutes at least */

static gboolean uploads_remove_lock = FALSE;
static gboolean uploads_shutting_down = FALSE;

static GtkTreeView *treeview_uploads = NULL;
static GtkListStore *store_uploads = NULL;
static GtkWidget *button_uploads_clear_completed = NULL;

/** hash table for fast handle -> GtkTreeIter mapping */
static GHashTable *upload_handles = NULL;
/** list of all *removed* uploads; contains the handles */
static GSList *sl_removed_uploads = NULL;

static void uploads_gui_update_upload_info(const gnet_upload_info_t *u);
static void uploads_gui_add_upload(gnet_upload_info_t *u);

static const char * const column_titles[UPLOADS_GUI_VISIBLE_COLUMNS] = {
	N_("Filename"),
	N_("Host"),
	N_("Loc"),
	N_("Size"),
	N_("Range"),
	N_("User-Agent"),
	N_("Progress"),
	N_("Status")
};

typedef struct remove_row_ctx {
	gboolean force;			/**< If false, rows will only be removed, if
							 **  their `entry_removal_timeout' has expired. */
	time_t now; 			/**< Current time, used to decide whether row
							 **  should be finally removed. */
	GSList *sl_remaining;	/**< Contains row data for not yet removed rows. */
} remove_row_ctx_t;


/**
 * Tries to fetch upload_row_data associated with the given upload handle.
 *
 * @return a pointer the upload_row_data.
 */
static inline upload_row_data_t *
find_upload(gnet_upload_t u)
{
	upload_row_data_t *rd;
	gpointer key;
	gboolean found;

	found = g_hash_table_lookup_extended(upload_handles, GUINT_TO_POINTER(u),
				NULL, &key);
	g_assert(found);
	rd = key;

	g_assert(NULL != rd);
	g_assert(rd->valid);
	g_assert(rd->handle == u);

	return rd;
}

/***
 *** Callbacks
 ***/

static gboolean
on_button_press_event(GtkWidget *unused_widget, GdkEventButton *event,
		gpointer unused_udata)
{
	(void) unused_widget;
	(void) unused_udata;

	if (3 == event->button) {
        /* Right click section (popup menu) */
		gtk_menu_popup(GTK_MENU(popup_uploads), NULL, NULL, NULL, NULL,
			event->button, event->time);
		return TRUE;
    }

	return FALSE;
}

/**
 * Callback: called when an upload is removed from the backend.
 *
 * Either immediately clears the upload from the frontend or just
 * set the upload_row_info->valid to FALSE, so we don't accidentally
 * try to use the handle to communicate with the backend.
 */
static void
upload_removed(gnet_upload_t uh, const gchar *reason,
		guint32 running, guint32 registered)
{
    upload_row_data_t *rd = NULL;

	(void) running;
	(void) registered;

    /* Invalidate row and remove it from the GUI if autoclear is on */
	rd = find_upload(uh);
	g_assert(NULL != rd);
	rd->valid = FALSE;
	gtk_widget_set_sensitive(button_uploads_clear_completed, TRUE);
	if (reason != NULL)
		gtk_list_store_set(store_uploads, &rd->iter, c_ul_status, reason, (-1));
	sl_removed_uploads = g_slist_prepend(sl_removed_uploads, rd);
	g_hash_table_remove(upload_handles, GUINT_TO_POINTER(uh));
	/* NB: rd MUST NOT be freed yet because it contains the GtkTreeIter! */
}



/**
 * Callback: called when an upload is added from the backend.
 *
 * Adds the upload to the gui.
 */
static void
upload_added(gnet_upload_t n, guint32 running, guint32 registered)
{
    gnet_upload_info_t *info;

	(void) running;
	(void) registered;

    info = guc_upload_get_info(n);
    uploads_gui_add_upload(info);
    guc_upload_free_info(info);
}

/**
 * Fetch the GUI row data associated with upload handle.
 */
upload_row_data_t *
uploads_gui_get_row_data(gnet_upload_t uhandle)
{
	return find_upload(uhandle);
}

/**
 * Callback: called when upload information was changed by the backend.
 * This updates the upload information in the gui.
 */
static void
upload_info_changed(gnet_upload_t u,
    guint32 running, guint32 registered)
{
    gnet_upload_info_t *info;

	(void) running;
	(void) registered;

    info = guc_upload_get_info(u);
    uploads_gui_update_upload_info(info);
    guc_upload_free_info(info);
}

#define COMPARE_FUNC(field) \
static gint CAT2(compare_,field)( \
	GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer user_data) \
{ \
	const upload_row_data_t *rd_a = NULL; \
	const upload_row_data_t *rd_b = NULL; \
	(void) user_data; \
	gtk_tree_model_get(model, a, c_ul_data, &rd_a, (-1)); \
	gtk_tree_model_get(model, b, c_ul_data, &rd_b, (-1)); \
	{

#define COMPARE_FUNC_END } }

COMPARE_FUNC(hosts)
	return host_addr_cmp(rd_a->addr, rd_b->addr);
COMPARE_FUNC_END

COMPARE_FUNC(sizes)
	return CMP(rd_b->size, rd_a->size);
COMPARE_FUNC_END

COMPARE_FUNC(ranges)
	filesize_t u = rd_a->range_end - rd_a->range_start;
	filesize_t v = rd_b->range_end - rd_b->range_start;
	gint s = CMP(v, u);
	return 0 != s ? s : CMP(rd_a->range_start, rd_b->range_start);
COMPARE_FUNC_END

/***
 *** Private functions
 ***/

static void
uploads_gui_update_upload_info(const gnet_upload_info_t *u)
{
	GdkColor *color = NULL;
    upload_row_data_t *rd = NULL;
	gnet_upload_status_t status;
	size_t range_len;
	gint progress;

	rd = find_upload(u->upload_handle);
	g_assert(NULL != rd);

	rd->last_update  = tm_time();

	if (!host_addr_equal(u->addr, rd->addr)) {
		rd->addr = u->addr;
		gtk_list_store_set(store_uploads, &rd->iter,
			c_ul_host, uploads_gui_host_string(u), (-1));
	}

	if (u->range_start != rd->range_start || u->range_end != rd->range_end) {
		static gchar str[256];

		rd->range_start  = u->range_start;
		rd->range_end  = u->range_end;

		if (u->range_start == 0 && u->range_end == 0)
			g_strlcpy(str, "...", sizeof str);
		else {
			range_len = gm_snprintf(str, sizeof str, "%s%s",
				u->partial ? "*" : "",
				short_size(u->range_end - u->range_start + 1,
					show_metric_units()));

			if ((guint) range_len < sizeof str) {
				if (u->range_start)
					range_len += gm_snprintf(&str[range_len],
									sizeof str - range_len,
									" @ %s", short_size(u->range_start,
												show_metric_units()));
				g_assert((guint) range_len < sizeof str);
			}
		}

		gtk_list_store_set(store_uploads, &rd->iter, c_ul_range, str, (-1));
	}

	if (u->file_size != rd->size) {
		rd->size = u->file_size;
		gtk_list_store_set(store_uploads, &rd->iter,
			c_ul_size, short_size(rd->size, show_metric_units()),
			(-1));
	}

	/* Exploit that u->name is an atom! */
	if (u->name != rd->name) {
		g_assert(NULL != u->name);
		if (NULL != rd->name)
			atom_str_free(rd->name);
		rd->name = atom_str_get(u->name);

		gtk_list_store_set(store_uploads, &rd->iter,
			c_ul_filename, rd->name,
			(-1));
	}

	/* Exploit that u->user_agent is an atom! */
	if (u->user_agent != rd->user_agent) {
		g_assert(NULL != u->user_agent);
		if (NULL != rd->user_agent)
			atom_str_free(rd->user_agent);
		rd->user_agent = atom_str_get(u->user_agent);
		gtk_list_store_set(store_uploads, &rd->iter,
			c_ul_agent, rd->user_agent,
			(-1));
	}

	if (u->country != rd->country) {
		rd->country = u->country;
		gtk_list_store_set(store_uploads, &rd->iter,
			c_ul_loc, iso3166_country_cc(rd->country),
			(-1));
	}

	guc_upload_get_status(u->upload_handle, &status);
	rd->status = status.status;

	progress = 100.0 * uploads_gui_progress(&status, rd);
	gtk_list_store_set(store_uploads, &rd->iter,
		c_ul_progress, CLAMP(progress, 0, 100),
		c_ul_status, uploads_gui_status_str(&status, rd),
		(-1));

	if (u->push) {
	    color = &(gtk_widget_get_style(GTK_WIDGET(treeview_uploads))
		      ->fg[GTK_STATE_INSENSITIVE]);
	    gtk_list_store_set(store_uploads, &rd->iter, c_ul_fg, color, (-1));
	}
}



/**
 * Adds the given upload to the gui.
 */
void
uploads_gui_add_upload(gnet_upload_info_t *u)
{
	gint range_len, progress;
	const gchar *titles[UPLOADS_GUI_VISIBLE_COLUMNS];
    upload_row_data_t *rd = walloc(sizeof *rd);
	gnet_upload_status_t status;
	static gchar size_tmp[256];

	memset(titles, 0, sizeof titles);

    rd->handle      = u->upload_handle;
    rd->range_start = u->range_start;
    rd->range_end   = u->range_end;
	rd->size		= u->file_size;
    rd->start_date  = u->start_date;
	rd->addr		= u->addr;
	rd->name		= NULL != u->name ? atom_str_get(u->name) : NULL;
	rd->country	    = u->country;
	rd->user_agent	= NULL != u->user_agent
						? atom_str_get(u->user_agent) : NULL;
	rd->push		= u->push;
	rd->valid		= TRUE;
    rd->gnet_addr   = zero_host_addr;
    rd->gnet_port   = 0;

	guc_upload_get_status(u->upload_handle, &status);
    rd->status = status.status;

    if (u->range_start == 0 && u->range_end == 0)
        titles[c_ul_range] =  "...";
    else {
		static gchar range_tmp[256];	/* MUST be static! */

        range_len = gm_snprintf(range_tmp, sizeof range_tmp, "%s%s",
			u->partial ? "*" : "",
            short_size(u->range_end - u->range_start + 1,
				show_metric_units()));

        if (u->range_start)
            range_len += gm_snprintf(
                &range_tmp[range_len], sizeof range_tmp - range_len,
                " @ %s", short_size(u->range_start, show_metric_units()));

        g_assert((guint) range_len < sizeof range_tmp);

        titles[c_ul_range] = range_tmp;
    }

	g_strlcpy(size_tmp, short_size(u->file_size, show_metric_units()),
		sizeof size_tmp);
    titles[c_ul_size] = size_tmp;

   	titles[c_ul_agent] = u->user_agent ? u->user_agent : "...";
	titles[c_ul_loc] = iso3166_country_cc(u->country);
	titles[c_ul_filename] = u->name ? u->name : "...";
	titles[c_ul_host] = uploads_gui_host_string(u);
	titles[c_ul_status] = uploads_gui_status_str(&status, rd);

	progress = 100.0 * uploads_gui_progress(&status, rd);
    gtk_list_store_append(store_uploads, &rd->iter);
    gtk_list_store_set(store_uploads, &rd->iter,
		c_ul_size, titles[c_ul_size],
		c_ul_range, titles[c_ul_range],
		c_ul_filename, titles[c_ul_filename],
		c_ul_host, titles[c_ul_host],
		c_ul_loc, titles[c_ul_loc],
		c_ul_agent, titles[c_ul_agent],
		c_ul_progress, CLAMP(progress, 0, 100),
		c_ul_status, titles[c_ul_status],
		c_ul_fg, NULL,
		c_ul_data, rd,
		(-1));
	g_hash_table_insert(upload_handles, GUINT_TO_POINTER(rd->handle), rd);
}

static void
add_column(gint column_id, GtkTreeIterCompareFunc sortfunc, GtkType column_type)
{
	GtkTreeViewColumn *column;
	GtkCellRenderer *renderer;
	gint xpad;

	g_assert(column_id >= 0 && (guint) column_id < UPLOADS_GUI_VISIBLE_COLUMNS);
	g_assert(NULL != treeview_uploads);
	g_assert(NULL != store_uploads);

	if (column_type == GTK_TYPE_CELL_RENDERER_PROGRESS) {
		xpad = 0;
		renderer = gtk_cell_renderer_progress_new();
		column = gtk_tree_view_column_new_with_attributes(
					_(column_titles[column_id]), renderer,
					"value", column_id,
					NULL);
	} else { /* if (column_type == GTK_TYPE_CELL_RENDERER_TEXT) { */
		xpad = GUI_CELL_RENDERER_XPAD;
		renderer = gtk_cell_renderer_text_new();
		gtk_cell_renderer_text_set_fixed_height_from_font(
			GTK_CELL_RENDERER_TEXT(renderer), 1);
		g_object_set(renderer,
			"foreground-set", TRUE,
			(void *) 0);
		column = gtk_tree_view_column_new_with_attributes(
					_(column_titles[column_id]), renderer,
					"foreground-gdk", c_ul_fg,
					"text", column_id,
					(void *) 0);
	}

	g_object_set(renderer,
		"xalign", (gfloat) 0.0,
		"xpad", xpad,
		"ypad", GUI_CELL_RENDERER_YPAD,
		(void *) 0);

	g_object_set(G_OBJECT(column),
		"fixed-width", 1,
		"min-width", 1,
		"reorderable", TRUE,
		"resizable", TRUE,
		"sizing", GTK_TREE_VIEW_COLUMN_FIXED,
		(void *) 0);

	gtk_tree_view_column_set_sort_column_id(column, column_id);
	gtk_tree_view_append_column(treeview_uploads, column);

	if (NULL != sortfunc)
		gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(store_uploads),
			column_id, sortfunc, GINT_TO_POINTER(column_id), NULL);
}

static GtkListStore *
create_uploads_model(void)
{
	static GType columns[c_ul_num];
	GtkListStore *store;
	guint i;

	STATIC_ASSERT(c_ul_num == G_N_ELEMENTS(columns));
#define SET(c, x) case (c): columns[i] = (x); break
	for (i = 0; i < G_N_ELEMENTS(columns); i++) {
		switch (i) {
		SET(c_ul_filename, G_TYPE_STRING);
		SET(c_ul_host, G_TYPE_STRING);
		SET(c_ul_loc, G_TYPE_STRING);
		SET(c_ul_size, G_TYPE_STRING);
		SET(c_ul_range, G_TYPE_STRING);
		SET(c_ul_agent, G_TYPE_STRING);
		SET(c_ul_progress, G_TYPE_INT);
		SET(c_ul_status, G_TYPE_STRING);
		SET(c_ul_fg, GDK_TYPE_COLOR);
		SET(c_ul_data, G_TYPE_POINTER);
		default:
			g_assert_not_reached();
		}
	}
#undef SET

	store = gtk_list_store_newv(G_N_ELEMENTS(columns), columns);
	return GTK_LIST_STORE(store);
}


/***
 *** Public functions
 ***/

void
uploads_gui_early_init(void)
{
    popup_uploads = create_popup_uploads();
}

void
uploads_gui_init(void)
{
	static const struct {
		gint id;
		GtkTreeIterCompareFunc sortfunc;
	} cols[] = {
		{ c_ul_filename, 	NULL },
		{ c_ul_host, 		compare_hosts },
		{ c_ul_loc, 		NULL },
		{ c_ul_size, 		compare_sizes },
		{ c_ul_range, 		compare_ranges },
		{ c_ul_agent, 		NULL },
		{ c_ul_progress, 	NULL },
		{ c_ul_status, 		NULL },
	};
	GtkTreeView *treeview;
	size_t i;

	STATIC_ASSERT(G_N_ELEMENTS(cols) == UPLOADS_GUI_VISIBLE_COLUMNS);
	store_uploads = create_uploads_model();

	button_uploads_clear_completed = lookup_widget(main_window,
		"button_uploads_clear_completed");
	treeview_uploads = treeview =
		GTK_TREE_VIEW(lookup_widget(main_window, "treeview_uploads"));
	gtk_tree_view_set_model(treeview, GTK_TREE_MODEL(store_uploads));

	for (i = 0; i < G_N_ELEMENTS(cols); i++)
		add_column(cols[i].id, cols[i].sortfunc,
			c_ul_progress == cols[i].id
				? GTK_TYPE_CELL_RENDERER_PROGRESS
				: GTK_TYPE_CELL_RENDERER_TEXT);

	tree_view_restore_widths(treeview_uploads, PROP_UPLOADS_COL_WIDTHS);
	tree_view_restore_visibility(treeview_uploads, PROP_UPLOADS_COL_VISIBLE);

	upload_handles = g_hash_table_new(NULL, NULL);

    guc_upload_add_upload_added_listener(upload_added);
    guc_upload_add_upload_removed_listener(upload_removed);
    guc_upload_add_upload_info_changed_listener
		(upload_info_changed);

	g_signal_connect(GTK_OBJECT(treeview), "button_press_event",
		G_CALLBACK(on_button_press_event), NULL);
}

static inline void
free_row_data(upload_row_data_t *rd)
{
	if (NULL != rd->user_agent) {
		atom_str_free(rd->user_agent);
		rd->user_agent = NULL;
	}
	if (NULL != rd->name) {
		atom_str_free(rd->name);
		rd->user_agent = NULL;
	}
	wfree(rd, sizeof *rd);
}

static inline void
free_handle(gpointer key, gpointer value,
	gpointer user_data)
{
	(void) key;
	(void) user_data;

	free_row_data(value);
}

static inline void
remove_row(upload_row_data_t *rd, remove_row_ctx_t *ctx)
{
	g_assert(NULL != rd);
	if (ctx->force || upload_should_remove(ctx->now, rd)) {
    	gtk_list_store_remove(store_uploads, &rd->iter);
		free_row_data(rd);
	} else
		ctx->sl_remaining = g_slist_prepend(ctx->sl_remaining, rd);
}

static inline void
update_row(gpointer key, gpointer data, gpointer user_data)
{
	time_t now = *(const time_t *) user_data;
	upload_row_data_t *rd = data;
	gnet_upload_status_t status;
	gint progress;

	g_assert(NULL != rd);
	g_assert(GPOINTER_TO_UINT(key) == rd->handle);
	if (delta_time(now, rd->last_update) < 2)
		return;

	rd->last_update = now;
	guc_upload_get_status(rd->handle, &status);
	progress = 100.0 * uploads_gui_progress(&status, rd);
	gtk_list_store_set(store_uploads, &rd->iter,
		c_ul_progress, CLAMP(progress, 0, 100),
		c_ul_status, uploads_gui_status_str(&status, rd),
		(-1));
}

/**
 * Update all the uploads at the same time.
 */
void
uploads_gui_update_display(time_t now)
{
    static time_t last_update;
	gint current_page;
	static GtkNotebook *notebook = NULL;

	/*
	 * Usually don't perform updates if nobody is watching.  However,
	 * we do need to perform periodic cleanup of dead entries or the
	 * memory usage will grow.  Perform an update every UPDATE_MIN minutes
	 * at least.
	 *		--RAM, 28/12/2003
	 */

	if (notebook == NULL)
		notebook = GTK_NOTEBOOK(lookup_widget(main_window, "notebook_main"));

	current_page = gtk_notebook_get_current_page(notebook);
	if (
		current_page != nb_main_page_uploads &&
		delta_time(now, last_update) < UPDATE_MIN
	) {
		return;
	}

    if (last_update != now) {
    	static gboolean locked = FALSE;
		remove_row_ctx_t ctx;

    	last_update = now;
		ctx.force = FALSE;
		ctx.now = now;
		ctx.sl_remaining = NULL;

		g_return_if_fail(!locked);
		locked = TRUE;

		/* Remove all rows with `removed' uploads. */
		G_SLIST_FOREACH_WITH_DATA(sl_removed_uploads, remove_row, &ctx);
		g_slist_free(sl_removed_uploads);
		sl_removed_uploads = ctx.sl_remaining;

		/* Update the status column for all active uploads. */
		g_hash_table_foreach(upload_handles, update_row, &now);

		if (NULL == sl_removed_uploads)
			gtk_widget_set_sensitive(button_uploads_clear_completed, FALSE);

		locked = FALSE;
	}
}

static gboolean
uploads_clear_helper(gpointer user_data)
{
	guint counter = 0;
    GSList *sl, *sl_remaining = NULL;
	remove_row_ctx_t ctx = { TRUE, 0, NULL };

	(void) user_data;

	if (uploads_shutting_down)
		return FALSE; /* Finished. */

	/* Remove all rows with `removed' uploads. */

	G_SLIST_FOREACH_WITH_DATA(sl_removed_uploads, remove_row, &ctx);
	g_slist_free(sl_removed_uploads);
	sl_removed_uploads = ctx.sl_remaining;

    for (sl = sl_removed_uploads; sl; sl = g_slist_next(sl_removed_uploads)) {
		remove_row((upload_row_data_t *) sl->data, &ctx);
		/* Interrupt and come back later to prevent GUI stalling. */
		if (0 == (++counter & 0x7f)) {
			/* Remember which elements haven't been traversed yet. */
			sl_remaining = g_slist_remove_link(sl, sl);
			break;
		}
    }
	/* The elements' data has been freed or is now referenced
	 * by ctx->remaining. */
	g_slist_free(sl_removed_uploads);
	sl_removed_uploads = g_slist_concat(ctx.sl_remaining, sl_remaining);

    if (NULL == sl_removed_uploads) {
		gtk_widget_set_sensitive(button_uploads_clear_completed, FALSE);
    	uploads_remove_lock = FALSE;
    	return FALSE; /* Finished. */
    }

    return TRUE; /* More rows to remove. */
}

void
uploads_gui_clear_completed(void)
{
	if (!uploads_remove_lock) {
		uploads_remove_lock = TRUE;
		gtk_timeout_add(100, uploads_clear_helper, store_uploads);
	}
}

/**
 * Unregister callbacks in the backend and clean up.
 */
void
uploads_gui_shutdown(void)
{
	uploads_shutting_down = TRUE;

	tree_view_save_widths(treeview_uploads, PROP_UPLOADS_COL_WIDTHS);
	tree_view_save_visibility(treeview_uploads, PROP_UPLOADS_COL_VISIBLE);

    guc_upload_remove_upload_added_listener(upload_added);
    guc_upload_remove_upload_removed_listener(upload_removed);
    guc_upload_remove_upload_info_changed_listener(upload_info_changed);

	gtk_list_store_clear(store_uploads);

	g_hash_table_foreach(upload_handles, free_handle, NULL);
	g_hash_table_destroy(upload_handles);
	upload_handles = NULL;
	G_SLIST_FOREACH(sl_removed_uploads, free_row_data);
	g_slist_free(sl_removed_uploads);
	sl_removed_uploads = NULL;
}

/* vi: set ts=4 sw=4 cindent: */
