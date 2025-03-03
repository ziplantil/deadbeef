//
//  medialibwidget.c
//  DeaDBeeF
//
//  Created by Alexey Yakovenko on 11/08/2021.
//  Copyright © 2021 Alexey Yakovenko. All rights reserved.
//


#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include "../../../deadbeef.h"
#include "../../../gettext.h"
#include "../../medialib/medialib.h"
#include "../prefwin/prefwin.h"
#include "../support.h"
#include "medialibwidget.h"
#include "medialibmanager.h"
#include "plcommon.h"

extern DB_functions_t *deadbeef;

typedef struct {
    ddb_gtkui_widget_t base;
    GtkTreeView *tree;
    GtkComboBoxText *selector;
    GtkEntry *search_entry;
    ddb_medialib_plugin_t *plugin;
    ddb_mediasource_source_t source;
    ddb_mediasource_list_selector_t *selectors;
    int active_selector;
    char *search_text;
    int listener_id;
    GtkTreeIter root_iter;
    ddb_medialib_item_t *item_tree;
} w_medialib_viewer_t;

enum {
    COL_TITLE,
    COL_TRACK,
};

static int
_item_comparator (const void *a, const void *b) {
    const ddb_medialib_item_t *item1 = *((ddb_medialib_item_t **)a);
    const ddb_medialib_item_t *item2 = *((ddb_medialib_item_t **)b);

     if (!item1->track || !item2->track) {
         return strcasecmp (item1->text, item2->text);
     }

     int n1 = atoi (deadbeef->pl_find_meta (item1->track, "track") ?: "0");
     int n2 = atoi (deadbeef->pl_find_meta (item2->track, "track") ?: "0");
     int d1 = atoi (deadbeef->pl_find_meta (item1->track, "disc") ?: "0") + 1;
     int d2 = atoi (deadbeef->pl_find_meta (item2->track, "disc") ?: "0") + 1;
     n1 = d1 * 10000 + n1;
     n2 = d2 * 10000 + n2;

    return n1-n2;
}

static ddb_medialib_item_t **
_sorted_children_from_item (ddb_medialib_item_t *item) {
    ddb_medialib_item_t **children = calloc (item->num_children, sizeof (ddb_medialib_item_t *));
    ddb_medialib_item_t *c = item->children;
    for (int i = 0; i < item->num_children; i++) {
        children[i] = c;
        c = c->next;
    }

    qsort (children, item->num_children, sizeof (ddb_medialib_item_t *), _item_comparator);

    return children;
}

static void
_add_items (w_medialib_viewer_t *mlv, GtkTreeIter *iter, ddb_medialib_item_t *item) {
    if (item == NULL) {
        return;
    }
    GtkTreeStore *store = GTK_TREE_STORE (gtk_tree_view_get_model (mlv->tree));

    ddb_medialib_item_t **sorted_items = _sorted_children_from_item (item);

    for (int i = 0; i < item->num_children; i++) {
        ddb_medialib_item_t *child_item = sorted_items[i];
        GtkTreeIter child;
        gtk_tree_store_append (store, &child, iter);
        if (child_item->num_children > 0) {
            size_t len = strlen(child_item->text) + 20;
            char *text = malloc (len + 20);
            snprintf (text, len, "%s (%d)", child_item->text, child_item->num_children);
            gtk_tree_store_set (store, &child, COL_TITLE, text, COL_TRACK, child_item->track, -1);
            free (text);
        }
        else {
            gtk_tree_store_set (store, &child, COL_TITLE, child_item->text, COL_TRACK, child_item->track, -1);
        }

        if (child_item->children != NULL) {
            _add_items (mlv, &child, child_item);
        }
    }

    free (sorted_items);
}

static gboolean
_medialib_state_did_change (void *user_data) {
    w_medialib_viewer_t *mlv = user_data;
    ddb_mediasource_state_t state = mlv->plugin->plugin.scanner_state (mlv->source);
    int enabled = mlv->plugin->plugin.get_source_enabled (mlv->source);
    GtkTreeStore *store = GTK_TREE_STORE (gtk_tree_view_get_model (mlv->tree));
    switch (state) {
    case DDB_MEDIASOURCE_STATE_IDLE:
        if (enabled) {
            char text[200];
            snprintf (text, sizeof (text), "%s (%d)", _("All Music"), mlv->item_tree ? mlv->item_tree->num_children : 0);
            gtk_tree_store_set (store, &mlv->root_iter, COL_TITLE, text, -1);
        }
        else {
            gtk_tree_store_set (store, &mlv->root_iter, COL_TITLE, _("Media library is disabled"), -1);
        }
        break;
    case DDB_MEDIASOURCE_STATE_LOADING:
        gtk_tree_store_set (store, &mlv->root_iter, COL_TITLE, _("Loading..."), -1);
        break;
    case DDB_MEDIASOURCE_STATE_SCANNING:
        gtk_tree_store_set (store, &mlv->root_iter, COL_TITLE, _("Scanning..."), -1);
        break;
    case DDB_MEDIASOURCE_STATE_INDEXING:
        gtk_tree_store_set (store, &mlv->root_iter, COL_TITLE, _("Indexing..."), -1);
        break;
    case DDB_MEDIASOURCE_STATE_SAVING:
        gtk_tree_store_set (store, &mlv->root_iter, COL_TITLE, _("Saving..."), -1);
        break;
    }

    return FALSE;
}

static void
_reload_content (w_medialib_viewer_t *mlv) {
    // populate the tree
    if (mlv->item_tree != NULL) {
        mlv->plugin->plugin.free_item_tree (mlv->source, mlv->item_tree);
        mlv->item_tree = NULL;
    }
    mlv->item_tree = mlv->plugin->plugin.create_item_tree (mlv->source, mlv->selectors[mlv->active_selector], mlv->search_text);

    // clear
    GtkTreeIter iter;
    GtkTreeStore *store = GTK_TREE_STORE (gtk_tree_view_get_model (mlv->tree));
    if (gtk_tree_model_iter_children (GTK_TREE_MODEL (store), &iter, &mlv->root_iter)) {
        while (gtk_tree_store_remove (store, &iter));
    }

    _add_items (mlv, &mlv->root_iter, mlv->item_tree);

    GtkTreePath *path = gtk_tree_path_new_from_indices (0, -1);
    gtk_tree_view_expand_row (mlv->tree, path, mlv->search_text != NULL);
    gtk_tree_path_free (path);

    _medialib_state_did_change (mlv);
}

static gboolean
_medialib_content_did_change (void *user_data) {
    w_medialib_viewer_t *mlv = user_data;
    if (mlv->plugin == NULL) {
        return FALSE;
    }
    _reload_content (mlv);
    return FALSE;
}

static void
_medialib_listener (ddb_mediasource_event_type_t event, void *user_data) {
    switch (event) {
    case DDB_MEDIASOURCE_EVENT_CONTENT_DID_CHANGE:
        g_idle_add (_medialib_content_did_change, user_data);
        break;
    case DDB_MEDIASOURCE_EVENT_STATE_DID_CHANGE:
    case DDB_MEDIASOURCE_EVENT_ENABLED_DID_CHANGE:
        g_idle_add (_medialib_state_did_change, user_data);
        break;
    case DDB_MEDIASOURCE_EVENT_SELECTORS_DID_CHANGE:
        break;
    }
}

static gboolean _selection_func (
                                 GtkTreeSelection  *selection,
                                 GtkTreeModel      *model,
                                 GtkTreePath       *path,
                                 gboolean           path_currently_selected,
                                 gpointer           data
                                 ) {
    gint *indices = gtk_tree_path_get_indices(path);

    int count = gtk_tree_path_get_depth(path);

    // don't select root
    if (count == 1 && indices[0] == 0) {
        return FALSE;
    }

    return TRUE;
}

static void
w_medialib_viewer_init (struct ddb_gtkui_widget_s *w) {
    // observe medialib source
    w_medialib_viewer_t *mlv = (w_medialib_viewer_t *)w;
    mlv->plugin = (ddb_medialib_plugin_t *)deadbeef->plug_get_for_id ("medialib");
    if (mlv->plugin == NULL) {
        return;
    }
    mlv->source = gtkui_medialib_get_source ();
    mlv->selectors = mlv->plugin->plugin.get_selectors_list (mlv->source);
    mlv->listener_id =  mlv->plugin->plugin.add_listener (mlv->source, _medialib_listener, mlv);

    for (int i = 0; mlv->selectors[i]; i++) {
        gtk_combo_box_text_append_text (mlv->selector, mlv->plugin->plugin.selector_name (mlv->source, mlv->selectors[i]));
    }
    gtk_combo_box_set_active (GTK_COMBO_BOX (mlv->selector), 0);
    mlv->active_selector = 0;

    // Root node
    GtkTreeStore *store = GTK_TREE_STORE (gtk_tree_view_get_model (mlv->tree));
    gtk_tree_store_append (store, &mlv->root_iter, NULL);

    GtkTreeSelection *selection = gtk_tree_view_get_selection (mlv->tree);
    gtk_tree_selection_set_select_function(selection, _selection_func, mlv, NULL);

    _reload_content (mlv);
}

static void
w_medialib_viewer_destroy (struct ddb_gtkui_widget_s *w) {
    w_medialib_viewer_t *mlv = (w_medialib_viewer_t *)w;
    if (mlv->source != NULL) {
        mlv->plugin->plugin.remove_listener (mlv->source, mlv->listener_id);
    }
    if (mlv->item_tree != NULL) {
        mlv->plugin->plugin.free_item_tree (mlv->source, mlv->item_tree);
        mlv->item_tree = NULL;
    }
    if (mlv->selectors != NULL) {
        mlv->plugin->plugin.free_selectors_list (mlv->source, mlv->selectors);
        mlv->selectors = NULL;
    }
    free (mlv->search_text);
    mlv->search_text = NULL;
}

static int
w_medialib_viewer_message (ddb_gtkui_widget_t *w, uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    return 0;
}

static GtkTreeViewColumn *
add_treeview_column (w_medialib_viewer_t *w, GtkTreeView *tree, int pos, int expand, int align_right, const char *title, int is_pixbuf)
{
    GtkCellRenderer *rend = NULL;
    GtkTreeViewColumn *col = NULL;
    if (is_pixbuf) {
        rend = gtk_cell_renderer_pixbuf_new ();
        col = gtk_tree_view_column_new_with_attributes (title, rend, "pixbuf", pos, NULL);
    }
    else {
        rend = gtk_cell_renderer_text_new ();
        col = gtk_tree_view_column_new_with_attributes (title, rend, "text", pos, NULL);
    }
    if (align_right) {
        g_object_set (rend, "xalign", 1.0, NULL);
    }

    gtk_tree_view_column_set_sizing (col, GTK_TREE_VIEW_COLUMN_AUTOSIZE);
    gtk_tree_view_column_set_expand (col, expand);
    gtk_tree_view_insert_column (GTK_TREE_VIEW (tree), col, pos);
    GtkWidget *label = gtk_label_new (title);
    gtk_tree_view_column_set_widget (col, label);
    gtk_widget_show (label);
    return col;
}

static void
_active_selector_did_change (GtkComboBox* self, gpointer user_data) {
    w_medialib_viewer_t *mlv = user_data;
    int active_selector = gtk_combo_box_get_active (self);
    if (mlv->active_selector != active_selector) {
        mlv->active_selector = active_selector;
        _reload_content (mlv);
    }
}

static void
_search_text_did_change (GtkEditable *editable, gpointer user_data) {
    w_medialib_viewer_t *mlv = user_data;

    const gchar *text = gtk_entry_get_text (mlv->search_entry);

    free (mlv->search_text);
    mlv->search_text = NULL;
    if (*text) {
        mlv->search_text = strdup (text);
    }

    _reload_content (mlv);
}

static int
_collect_tracks_from_iter (GtkTreeModel *model, GtkTreeIter *iter, ddb_playItem_t **tracks, int append_position) {
    // is it a track?
    GValue value = {0};
    gtk_tree_model_get_value (model, iter, COL_TRACK, &value);
    ddb_playItem_t *track = g_value_get_pointer (&value);
    g_value_unset (&value);

    if (track) {
        if (tracks != NULL) {
            tracks[append_position] = track;
        }
        return 1;
    }

    int count = 0;
    // recurse into children
    GtkTreeIter child;
    if (gtk_tree_model_iter_children (model, &child, iter)) {
        do {
            int appended_count = _collect_tracks_from_iter (model, &child, tracks, append_position);
            count += appended_count;
            append_position += appended_count;
        } while (gtk_tree_model_iter_next (model, &child));
    }

    return count;
}

static int
_collect_selected_tracks (GtkTreeModel *model, GtkTreeSelection *selection, ddb_playItem_t **tracks, int append_position) {
    GList *selected_rows = gtk_tree_selection_get_selected_rows (selection, NULL);

    int count = 0;

    for (GList *row = selected_rows; row; row = row->next) {
        GtkTreePath *path = (GtkTreePath *)row->data;
        GtkTreeIter iter;
        if (!gtk_tree_model_get_iter (model, &iter, path)) {
            continue;
        }

        int appended_count = _collect_tracks_from_iter (model, &iter, tracks, append_position);
        count += appended_count;
        append_position += appended_count;
    }

    g_list_free_full (selected_rows, (GDestroyNotify) gtk_tree_path_free);
    return count;
}

static void _append_tracks_to_playlist (ddb_playItem_t **tracks, int count, ddb_playlist_t *plt) {
    ddb_playItem_t *prev = deadbeef->plt_get_tail_item (plt, PL_MAIN);
    for (int i = 0; i < count; i++) {
        ddb_playItem_t *it = deadbeef->pl_item_alloc ();
        deadbeef->pl_item_copy (it, tracks[i]);
        deadbeef->plt_insert_item (plt, prev, it);
        if (prev != NULL) {
            deadbeef->pl_item_unref (prev);
        }
        prev = it;
    }
    if (prev != NULL) {
        deadbeef->pl_item_unref (prev);
    }
    prev = NULL;
}

static ddb_playlist_t *
_get_target_playlist (void) {
    ddb_playlist_t *plt = NULL;
    if (deadbeef->conf_get_int ("cli_add_to_specific_playlist", 1)) {
        char str[200];
        deadbeef->conf_get_str ("cli_add_playlist_name", "Default", str, sizeof (str));
        plt = deadbeef->plt_find_by_name (str);
        if (plt == NULL) {
            plt = deadbeef->plt_append (str);
        }
    }
    return plt;
}

static void
_treeview_row_did_activate (GtkTreeView* self, GtkTreePath* path, GtkTreeViewColumn* column, gpointer user_data) {
    w_medialib_viewer_t *mlv = user_data;

    GtkTreeModel *model = GTK_TREE_MODEL (gtk_tree_view_get_model (mlv->tree));
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter (model, &iter, path)) {
        return;
    }

    ddb_playlist_t *curr_plt = _get_target_playlist ();
    if (curr_plt == NULL) {
        return;
    }

    deadbeef->plt_set_curr (curr_plt);
    deadbeef->plt_clear (curr_plt);

    GtkTreeSelection *selection = gtk_tree_view_get_selection (self);

    // count selected tracks
    int count = _collect_selected_tracks (model, selection, NULL, 0);

    if (count > 0) {
        // create array of tracks
        ddb_playItem_t **tracks = NULL;
        tracks = calloc (count, sizeof (ddb_playItem_t *));
        _collect_selected_tracks (model, selection, tracks, 0);

        _append_tracks_to_playlist (tracks, count, curr_plt);

        free (tracks);

        deadbeef->sendmessage (DB_EV_PLAYLISTCHANGED, 0, DDB_PLAYLIST_CHANGE_CONTENT, 0);
        deadbeef->sendmessage (DB_EV_PLAY_NUM, 0, 0, 0);
    }

    deadbeef->plt_unref (curr_plt);
}

static gboolean
_select_at_position (GtkTreeView *treeview, gint x, gint y) {
    GtkTreeSelection *selection = gtk_tree_view_get_selection (treeview);
    GtkTreeModel *model = GTK_TREE_MODEL (gtk_tree_view_get_model (treeview));

    GtkTreePath *path;
    if (!gtk_tree_view_get_path_at_pos (treeview,
                                       x, y,
                                       &path, NULL, NULL, NULL)) {
        gtk_tree_selection_unselect_all (selection);
        return FALSE;
    }

    GtkTreeIter iter;
    gtk_tree_model_get_iter (model, &iter, path);
    if (!gtk_tree_selection_iter_is_selected (selection, &iter)) {
        gtk_tree_selection_unselect_all (selection);
        gtk_tree_selection_select_path (selection, path);
    }
    gtk_tree_path_free (path);
    return TRUE;
}

static void
_trkproperties_did_change_tracks (void *user_data) {
    w_medialib_viewer_t *mlv = user_data;
    mlv->plugin->plugin.refresh (mlv->source);
}

static trkproperties_delegate_t _trkproperties_delegate = {
    .trkproperties_did_update_tracks = _trkproperties_did_change_tracks,
    .trkproperties_did_reload_metadata = _trkproperties_did_change_tracks,
    .trkproperties_did_delete_files = _trkproperties_did_change_tracks,
};

gboolean
_treeview_row_mousedown (GtkWidget* self, GdkEventButton *event, gpointer user_data) {
    if (w_get_design_mode ()) {
        return FALSE;
    }

    if (event->type != GDK_BUTTON_PRESS || (event->button != 3 && event->button != 2)) {
        return FALSE;
    }

    w_medialib_viewer_t *mlv = user_data;
    GtkTreeModel *model = GTK_TREE_MODEL (gtk_tree_view_get_model (mlv->tree));
    GtkTreeSelection *selection = gtk_tree_view_get_selection (mlv->tree);

    if (!_select_at_position (mlv->tree, event->x, event->y)) {
        return FALSE;
    }

    // count selected tracks
    int count = _collect_selected_tracks (model, selection, NULL, 0);

    // create array of tracks
    ddb_playItem_t **tracks = NULL;
    if (count == 0) {
        return TRUE;
    }

    tracks = calloc (count, sizeof (ddb_playItem_t *));
    _collect_selected_tracks (model, selection, tracks, 0);

    // context menu
    if (event->button == 3) {
        _trkproperties_delegate.user_data = mlv;
        list_context_menu_with_track_list (tracks, count, &_trkproperties_delegate);
    }
    // append to playlist
    else if (event->button == 2 && count > 0) {
        ddb_playlist_t *curr_plt = _get_target_playlist ();
        if (curr_plt != NULL) {
            deadbeef->plt_set_curr (curr_plt);
            _append_tracks_to_playlist (tracks, count, curr_plt);
            deadbeef->sendmessage (DB_EV_PLAYLISTCHANGED, 0, DDB_PLAYLIST_CHANGE_CONTENT, 0);
        }
    }
    free (tracks);

    return TRUE;
}

static void
_configure_did_activate (GtkButton* self, gpointer user_data) {
    prefwin_run (PREFWIN_TAB_INDEX_MEDIALIB);
}

static void
_drag_data_get (
               GtkWidget* widget,
               GdkDragContext* context,
               GtkSelectionData* selection_data,
               guint info,
               guint time_
                ) {
    GtkTreeModel *model = GTK_TREE_MODEL (gtk_tree_view_get_model (GTK_TREE_VIEW (widget)));
    GtkTreeSelection *selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (widget));

    int count = _collect_selected_tracks (model, selection, NULL, 0);
    if (count == 0) {
        return;
    }
    ddb_playItem_t **tracks = calloc (count, sizeof (ddb_playItem_t *));
    _collect_selected_tracks (model, selection, tracks, 0);

    for (int i = 0; i < count; i++) {
        deadbeef->pl_item_ref (tracks[i]);
    }

    GdkAtom type = gtk_selection_data_get_target (selection_data);
    gtk_selection_data_set (selection_data,
                           type,
                           sizeof (void *) * 8,
                           (const guchar *)tracks,
                           count * sizeof (ddb_playItem_t *));

    free (tracks);
    tracks = NULL;
}

ddb_gtkui_widget_t *
w_medialib_viewer_create (void) {
    w_medialib_viewer_t *w = calloc (1, sizeof (w_medialib_viewer_t));

    w->base.widget = gtk_event_box_new ();
    w->base.init = w_medialib_viewer_init;
    w->base.destroy = w_medialib_viewer_destroy;
    w->base.message = w_medialib_viewer_message;

    gtk_widget_set_can_focus (w->base.widget, FALSE);

    ddb_medialib_plugin_t *plugin = (ddb_medialib_plugin_t *)deadbeef->plug_get_for_id ("medialib");
    if (plugin == NULL) {
        GtkWidget *label = gtk_label_new(_("Media Library plugin is unavailable."));
        gtk_widget_show (label);
        gtk_container_add (GTK_CONTAINER (w->base.widget), label);
        return (ddb_gtkui_widget_t *)w;
    }

    GtkWidget *vbox = gtk_vbox_new (FALSE, 8);
    gtk_widget_show (vbox);
    gtk_container_add (GTK_CONTAINER (w->base.widget), vbox);

    GtkWidget *configure_wrap_hbox = gtk_hbox_new (FALSE, 8);
    gtk_widget_show (configure_wrap_hbox);
    gtk_box_pack_start (GTK_BOX (vbox), configure_wrap_hbox, FALSE, TRUE, 0);

    GtkWidget *configure_hbox = gtk_hbox_new (FALSE, 8);
    gtk_widget_show (configure_hbox);
    gtk_box_pack_start (GTK_BOX (configure_wrap_hbox), configure_hbox, TRUE, TRUE, 20);

    w->selector = GTK_COMBO_BOX_TEXT (gtk_combo_box_text_new ());
    gtk_widget_show (GTK_WIDGET (w->selector));
    gtk_box_pack_start (GTK_BOX (configure_hbox), GTK_WIDGET (w->selector), TRUE, TRUE, 0);

    GtkWidget *configure_button = gtk_button_new_with_label (_("Configure"));
    gtk_widget_show (configure_button);
    gtk_box_pack_start (GTK_BOX (configure_hbox), configure_button, FALSE, TRUE, 0);

    GtkWidget *search_hbox = gtk_hbox_new (FALSE, 8);
    gtk_widget_show (search_hbox);
    gtk_box_pack_start (GTK_BOX (vbox), search_hbox, FALSE, TRUE, 0);

    w->search_entry = GTK_ENTRY (gtk_entry_new ());
#if GTK_CHECK_VERSION (3,2,0)
    gtk_entry_set_placeholder_text (w->search_entry, _("Search"));
#endif
    gtk_widget_show (GTK_WIDGET (w->search_entry));
    gtk_box_pack_start (GTK_BOX (search_hbox), GTK_WIDGET (w->search_entry), TRUE, TRUE, 20);

    GtkWidget *scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_widget_set_can_focus (scroll, FALSE);
    gtk_widget_show (scroll);
    gtk_box_pack_start (GTK_BOX (vbox), scroll, TRUE, TRUE, 0);

    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scroll), GTK_SHADOW_ETCHED_IN);
    w->tree = GTK_TREE_VIEW (gtk_tree_view_new ());
    gtk_tree_view_set_reorderable (GTK_TREE_VIEW (w->tree), FALSE);
    gtk_tree_view_set_enable_search (GTK_TREE_VIEW (w->tree), TRUE);
    GtkTreeSelection *sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (w->tree));
    gtk_tree_selection_set_mode (sel, GTK_SELECTION_BROWSE);
    gtk_widget_show (GTK_WIDGET (w->tree));

    gtk_container_add (GTK_CONTAINER (scroll), GTK_WIDGET (w->tree));

    GtkTreeStore *store = gtk_tree_store_new (2, G_TYPE_STRING, G_TYPE_POINTER);
    gtk_tree_view_set_model (GTK_TREE_VIEW (w->tree), GTK_TREE_MODEL (store));

    gtk_tree_view_set_rules_hint (GTK_TREE_VIEW (w->tree), TRUE);
    add_treeview_column (w, GTK_TREE_VIEW (w->tree), COL_TITLE, 1, 0, "", 0);

    gtk_tree_view_set_headers_clickable (GTK_TREE_VIEW (w->tree), FALSE);
    gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (w->tree), FALSE);

    GtkTreeSelection *selection = gtk_tree_view_get_selection (w->tree);
    gtk_tree_selection_set_mode (selection, GTK_SELECTION_MULTIPLE);

    g_signal_connect ((gpointer)w->selector, "changed", G_CALLBACK (_active_selector_did_change), w);
    g_signal_connect ((gpointer)w->search_entry, "changed", G_CALLBACK (_search_text_did_change), w);
    g_signal_connect ((gpointer)w->tree, "row-activated", G_CALLBACK (_treeview_row_did_activate), w);
    g_signal_connect ((gpointer)w->tree, "button_press_event", G_CALLBACK (_treeview_row_mousedown), w);
    g_signal_connect ((gpointer)configure_button, "clicked", G_CALLBACK (_configure_did_activate), w);

    GtkTargetEntry entry = {
        .target = TARGET_PLAYITEM_POINTERS,
        .flags = GTK_TARGET_SAME_APP,
        .info = 0
    };
    gtk_drag_source_set (GTK_WIDGET (w->tree), GDK_BUTTON1_MASK, &entry, 1, GDK_ACTION_COPY);

    g_signal_connect ((gpointer) w->tree, "drag_data_get", G_CALLBACK (_drag_data_get), w);

    w_override_signals (w->base.widget, w);

    return (ddb_gtkui_widget_t *)w;
}
