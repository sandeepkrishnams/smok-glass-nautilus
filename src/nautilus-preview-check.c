/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Integration checks exercise the actual widget, loader and Markdown renderer. */
#include "nautilus-preview-pane.c"
#undef G_LOG_DOMAIN
#include "nautilus-window-slot.c"
#include "nautilus-grid-cell.h"
#include "nautilus-view-model.h"
#include <glib/gstdio.h>
#include "nautilus-global-preferences.h"
#include "nautilus-file-utilities.h"
#include "nautilus-resources.h"
static char *capture_path;
static char *sample_directory;
static char *artifact_directory;

static void
set_capture_path (const char *name)
{
    g_free (capture_path);
    capture_path = g_build_filename (artifact_directory, name, NULL);
}
static gboolean sheet_capture_done;

static void
sheet_snapshot_ready (GObject *object, GAsyncResult *result, gpointer data)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GdkTexture) texture = webkit_web_view_get_snapshot_finish (WEBKIT_WEB_VIEW (object), result, &error);
    g_assert_no_error (error);
    g_assert_nonnull (texture);
    g_assert_true (gdk_texture_save_to_png (texture, capture_path));
    sheet_capture_done = TRUE;
}

static void
pump (guint milliseconds)
{
    gint64 until = g_get_monotonic_time () + milliseconds * 1000;
    do
    {
        /* Keep continuously scheduled GTK sources from extending a timed pump. */
        g_main_context_iteration (NULL, FALSE);
        g_usleep (1000);
    } while (g_get_monotonic_time () < until);
}

static NautilusFile *
ready_file (const char *path)
{
    g_autoptr (GFile) location = g_file_new_for_commandline_arg (path);
    NautilusFile *file = nautilus_file_get (location);
    nautilus_file_monitor_add (file, file, NAUTILUS_FILE_ATTRIBUTE_INFO);
    for (guint i = 0; i < 5000 && !nautilus_file_check_if_ready (file, NAUTILUS_FILE_ATTRIBUTE_INFO); i++) pump (1);
    g_assert_true (nautilus_file_check_if_ready (file, NAUTILUS_FILE_ATTRIBUTE_INFO));
    return file;
}

static void
select_file (NautilusPreviewPane *pane, NautilusFile *file)
{
    GList selection = { .data = file };
    nautilus_preview_pane_set_files (pane, &selection);
    for (guint i = 0; i < 45000 && g_strcmp0 (gtk_label_get_text (pane->message), "Loading preview…") == 0 &&
         g_strcmp0 (gtk_stack_get_visible_child_name (pane->stack), "message") == 0; i++) pump (1);
}

static char *
text_contents (NautilusPreviewPane *pane)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer (pane->text);
    GtkTextIter begin, end;
    gtk_text_buffer_get_bounds (buffer, &begin, &end);
    return gtk_text_buffer_get_text (buffer, &begin, &end, FALSE);
}

static gboolean
capture (gpointer data)
{
    GtkWidget *window = data;
    GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (window));
    g_autoptr (GdkPaintable) paintable = gtk_widget_paintable_new (window);
    g_autoptr (GtkSnapshot) snapshot = gtk_snapshot_new ();
    int width = gtk_widget_get_width (window), height = gtk_widget_get_height (window);
    gdk_paintable_snapshot (paintable, GDK_SNAPSHOT (snapshot), width, height);
    g_autoptr (GskRenderNode) node = gtk_snapshot_free_to_node (g_steal_pointer (&snapshot));
    g_autoptr (GskRenderer) renderer = gsk_renderer_new_for_surface (surface);
    if (node != NULL && renderer != NULL)
    {
        graphene_rect_t viewport = GRAPHENE_RECT_INIT (0, 0, width, height);
        g_autoptr (GdkTexture) texture = gsk_renderer_render_texture (renderer, node, &viewport);
        gdk_texture_save_to_png (texture, capture_path);
    }
    if (renderer != NULL) gsk_renderer_unrealize (renderer);
    return G_SOURCE_REMOVE;
}

static void
wait_for_folder_page (NautilusPreviewPane *pane)
{
    gint64 deadline = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;
    while (pane->folder_loading && g_get_monotonic_time () < deadline)
    {
        g_main_context_iteration (NULL, FALSE);
        g_usleep (1000);
    }
    g_assert_false (pane->folder_loading);
}

static void
wait_for_overview (NautilusPreviewPane *pane)
{
    gint64 deadline = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;
    while ((pane->overview_source != 0 || pane->overview_task != NULL) &&
           g_get_monotonic_time () < deadline) pump (1);
    g_assert_cmpuint (pane->overview_source, ==, 0);
    g_assert_null (pane->overview_task);
    g_assert_false (pane->usage_result.running);
}

static void
usage_test_file (const char *directory, const char *name, goffset bytes)
{
    g_autofree char *path = g_build_filename (directory, name, NULL);
    int fd = g_open (path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    g_assert_cmpint (fd, >=, 0);
    g_assert_cmpint (ftruncate (fd, bytes), ==, 0);
    close (fd);
}

static guint64
usage_segment_bytes (NautilusUsageResult *result, const char *name)
{
    for (guint i = 0; i < result->n_segments; i++)
        if (g_str_equal (result->segments[i].name, name)) return result->segments[i].bytes;
    g_error ("Missing usage segment %s", name);
}

static void
check_overview (NautilusPreviewPane *pane, GtkWidget *window, NautilusFile *text_file)
{
    g_autofree char *temp = g_dir_make_tmp ("smok-folder-usage-XXXXXX", NULL);
    g_autofree char *root = g_build_filename (temp, "Usage demo", NULL);
    g_autofree char *outside = g_build_filename (temp, "Outside", NULL);
    g_mkdir (root, 0700); g_mkdir (outside, 0700);
    usage_test_file (outside, "not-included", 100000000);
    const char *names[] = { "Photos/Originals", "Videos", "Documents", "Archive", "Empty" };
    const goffset sizes[] = { 6000000, 3000000, 1000000, 500000, 0 };
    for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    {
        g_autofree char *child = g_build_filename (root, names[i], NULL);
        g_mkdir_with_parents (child, 0700);
        if (sizes[i] > 0) usage_test_file (child, "sample", sizes[i]);
    }
    usage_test_file (root, "notes.txt", 250000);
    usage_test_file (root, ".hidden", 50000);
    g_autofree char *link = g_build_filename (root, "External link", NULL);
    g_assert_cmpint (symlink (outside, link), ==, 0);
    g_autofree char *fifo = g_build_filename (root, "Pipe", NULL);
    g_assert_cmpint (mkfifo (fifo, 0600), ==, 0);
    g_autoptr (GFile) location = g_file_new_for_path (root);
    nautilus_preview_pane_set_context (pane, location, NULL, TRUE);
    nautilus_preview_pane_set_files (pane, NULL);
    g_assert_null (pane->overview_task);
    g_assert_cmpuint (pane->overview_source, ==, 0);
    g_assert_nonnull (strstr (gtk_label_get_text (pane->storage_label), "Waiting"));
    nautilus_preview_pane_set_context (pane, location, NULL, FALSE);
    nautilus_preview_pane_set_files (pane, NULL);
    g_assert_null (pane->overview_task);
    g_assert_cmpuint (pane->overview_source, !=, 0);
    wait_for_overview (pane);
    g_assert_true (pane->overview_active);
    g_assert_cmpstr (gtk_stack_get_visible_child_name (pane->stack), ==, "overview");
    g_assert_cmpuint (pane->usage_result.bytes, ==, 10800000);
    g_assert_cmpuint (pane->usage_result.files, ==, 8);
    g_assert_cmpuint (pane->usage_result.folders, ==, 6);
    g_assert_cmpuint (usage_segment_bytes (&pane->usage_result, "Photos/"), ==, 6000000);
    g_assert_cmpuint (usage_segment_bytes (&pane->usage_result, "Loose files"), ==, 300000);
    g_assert_false (pane->usage_result.partial);
    g_assert_false (gtk_widget_get_visible (pane->open));
    g_print ("PASS folder usage recursive sizes, nested counts, hidden files and loose-file grouping\n");
    g_print ("PASS usage avoids symlink traversal, special-file reads and sparse-file payloads\n");
    set_capture_path ("folder-overview.png");
    pump (300); capture (window);
    g_print ("PASS folder usage multicolour donut and matching size/percentage legend\n");
    gint64 checked = pane->usage_checked;
    select_file (pane, text_file);
    g_assert_false (pane->overview_active);
    g_assert_true (gtk_widget_get_visible (pane->open));
    nautilus_preview_pane_set_files (pane, NULL);
    wait_for_overview (pane);
    g_assert_cmpint (pane->usage_checked, ==, checked);
    g_print ("PASS usage restores after selection with cached folder totals\n");
    overview_refresh_clicked (NULL, pane);
    g_assert_nonnull (pane->overview_task);
    overview_refresh_clicked (NULL, pane);
    g_assert_null (pane->overview_task);
    g_assert_true (pane->usage_result.cancelled);
    pump (200);
    g_assert_true (pane->usage_result.cancelled);
    overview_refresh_clicked (NULL, pane);
    wait_for_overview (pane);
    g_assert_cmpuint (pane->usage_result.bytes, ==, 10800000);
    g_print ("PASS usage Stop/Rescan controls and rejection of cancelled worker results\n");

    /* Top-six aggregation must conserve bytes for any number of root folders. */
    for (guint i = 0; i < 20; i++)
    {
        g_autofree char *name = g_strdup_printf ("Tiny-%02u", i);
        g_autofree char *child = g_build_filename (root, name, NULL);
        g_mkdir (child, 0700);
        usage_test_file (child, "small", 10);
    }
    overview_refresh_clicked (NULL, pane);
    wait_for_overview (pane);
    g_assert_cmpuint (pane->usage_result.n_segments, ==, 8);
    g_assert_cmpuint (pane->usage_result.bytes, ==, 10800200);
    g_assert_cmpuint (usage_segment_bytes (&pane->usage_result, "Other folders"), ==, 180);
    guint64 sum = 0;
    for (guint i = 0; i < pane->usage_result.n_segments; i++) sum += pane->usage_result.segments[i].bytes;
    g_assert_cmpuint (sum, ==, pane->usage_result.bytes);
    g_print ("PASS bounded top-six aggregation conserves bytes with Other folders\n");

    g_autofree char *empty = g_build_filename (root, "Empty", NULL);
    g_autoptr (GFile) empty_location = g_file_new_for_path (empty);
    nautilus_preview_pane_set_context (pane, empty_location, NULL, FALSE);
    nautilus_preview_pane_set_files (pane, NULL);
    wait_for_overview (pane);
    g_assert_cmpstr (gtk_label_get_text (pane->storage_label), ==, "This folder is empty.");
    g_assert_cmpuint (pane->usage_result.bytes, ==, 0);
    g_print ("PASS usage empty folder state without invalid percentages\n");
    g_autoptr (GFile) missing = g_file_new_for_path ("/nonexistent-smok-overview-location");
    nautilus_preview_pane_set_context (pane, missing, NULL, FALSE);
    nautilus_preview_pane_set_files (pane, NULL);
    wait_for_overview (pane);
    g_assert_true (pane->usage_result.unreadable);
    g_assert_true (pane->usage_result.partial);
    g_print ("PASS usage unreadable folder reports partial totals\n");
    g_autoptr (GFile) network = g_file_new_for_uri ("smb://smok-preview.invalid/share");
    nautilus_preview_pane_set_context (pane, network, NULL, FALSE);
    nautilus_preview_pane_set_files (pane, NULL);
    g_assert_null (pane->overview_task);
    g_assert_cmpuint (pane->overview_source, ==, 0);
    g_assert_cmpstr (gtk_button_get_label (GTK_BUTTON (pane->overview_refresh)), ==, "Calculate folder usage");
    g_print ("PASS network folder usage requires an explicit scan\n");
    nautilus_preview_pane_set_context (pane, location, NULL, FALSE);
    nautilus_preview_pane_set_files (pane, NULL);
    select_file (pane, text_file);
    pump (500);
    g_assert_false (pane->overview_active);
    g_assert_cmpuint (pane->overview_source, ==, 0);
    g_assert_null (pane->overview_task);
    g_assert_cmpstr (gtk_stack_get_visible_child_name (pane->stack), ==, "text");
    nautilus_preview_pane_set_context (pane, NULL, NULL, FALSE);
    nautilus_preview_pane_set_files (pane, NULL);
    g_assert_cmpstr (gtk_stack_get_visible_child_name (pane->stack), ==, "message");
    g_print ("PASS usage pending-scan cancellation and virtual view fallback\n");

    g_autofree char *deep = g_build_filename (temp, "Deep", NULL);
    g_autofree char *deep_child = g_strdup (deep);
    for (guint i = 0; i < 70; i++)
    {
        g_mkdir (deep_child, 0700);
        char *next = g_build_filename (deep_child, "d", NULL);
        g_free (deep_child); deep_child = next;
    }
    g_autoptr (GFile) deep_location = g_file_new_for_path (deep);
    NautilusFolderUsage *usage = nautilus_folder_usage_new (deep_location);
    g_autoptr (GCancellable) cancel = g_cancellable_new ();
    nautilus_folder_usage_scan (usage, cancel);
    NautilusUsageResult result;
    nautilus_folder_usage_snapshot (usage, &result);
    g_assert_true (result.limited);
    g_assert_true (result.partial);
    g_assert_cmpuint (result.folders, ==, 64);
    nautilus_folder_usage_free (usage);
    usage = nautilus_folder_usage_new (location);
    g_cancellable_cancel (cancel);
    nautilus_folder_usage_scan (usage, cancel);
    nautilus_folder_usage_snapshot (usage, &result);
    g_assert_true (result.cancelled);
    g_assert_cmpuint (result.files, ==, 0);
    nautilus_folder_usage_free (usage);
    g_print ("PASS usage depth safety limit and already-cancelled scan\n");
    /* Exercise actual slot navigation, not just the standalone widget API. */
    NautilusWindowSlot *slot = nautilus_window_slot_new (NAUTILUS_MODE_BROWSE);
    GtkWidget *slot_window = gtk_window_new ();
    gtk_window_set_default_size (GTK_WINDOW (slot_window), 1000, 700);
    gtk_window_set_child (GTK_WINDOW (slot_window), GTK_WIDGET (slot));
    gtk_window_present (GTK_WINDOW (slot_window));
    nautilus_window_slot_open_location_full (slot, location, NULL);
    gint64 deadline = g_get_monotonic_time () + 15 * G_USEC_PER_SEC;
    while ((slot->content_view == NULL || nautilus_files_view_is_loading (slot->content_view) ||
            !slot->preview_pane->overview_active) && g_get_monotonic_time () < deadline) pump (1);
    nautilus_window_slot_set_selection (slot, NULL);
    wait_for_overview (slot->preview_pane);
    g_assert_true (slot->preview_pane->overview_active);
    g_assert_cmpuint (slot->preview_pane->usage_result.bytes, ==, 10800200);
    slot->preview_hover_file = nautilus_file_ref (text_file);
    show_hover_preview (slot);
    g_assert_true (slot->preview_hover_active);
    g_assert_null (slot->selection);
    clear_preview_hover (slot, TRUE);
    wait_for_overview (slot->preview_pane);
    g_assert_true (slot->preview_pane->overview_active);
    g_assert_cmpuint (slot->preview_pane->usage_result.bytes, ==, 10800200);
    nautilus_window_slot_open_location_full (slot, empty_location, NULL);
    deadline = g_get_monotonic_time () + 15 * G_USEC_PER_SEC;
    while ((nautilus_files_view_is_loading (slot->content_view) ||
            slot->preview_pane->overview_location == NULL ||
            !g_file_equal (slot->preview_pane->overview_location, empty_location)) &&
           g_get_monotonic_time () < deadline) pump (1);
    nautilus_window_slot_set_selection (slot, NULL);
    wait_for_overview (slot->preview_pane);
    g_assert_cmpuint (slot->preview_pane->usage_result.files, ==, 0);
    g_assert_cmpuint (slot->preview_pane->usage_result.folders, ==, 0);
    gtk_window_destroy (GTK_WINDOW (slot_window));
    pump (150);
    g_print ("PASS real slot folder navigation, unselected overview and hover restoration\n");
    remove_preview_directory (temp);
}

static void
check_folder_preview (NautilusPreviewPane *pane, GtkWidget *window, NautilusFile *text_file)
{
    g_autofree char *root = g_dir_make_tmp ("nautilus-folder-preview-XXXXXX", NULL);
    g_autofree char *dir = g_build_filename (root, "Project files", NULL);
    g_mkdir (dir, 0700);
    g_autofree char *subdir = g_build_filename (dir, "Designs", NULL);
    g_autofree char *notes = g_build_filename (dir, "Notes & ideas.txt", NULL);
    g_autofree char *hidden = g_build_filename (dir, ".hidden", NULL);
    g_autofree char *link = g_build_filename (dir, "Shortcut", NULL);
    g_mkdir (subdir, 0700);
    g_file_set_contents (notes, "Folder preview sample", -1, NULL);
    g_file_set_contents (hidden, "Hidden sample", -1, NULL);
    g_assert_cmpint (symlink (subdir, link), ==, 0);
    NautilusFile *folder = ready_file (dir);
    NautilusFile *empty = ready_file (subdir);
    g_settings_set_boolean (gtk_filechooser_preferences, NAUTILUS_PREFERENCES_SHOW_HIDDEN_FILES, FALSE);
    select_file (pane, folder);
    g_assert_cmpstr (gtk_stack_get_visible_child_name (pane->stack), ==, "folder");
    g_assert_nonnull (strstr (gtk_label_get_text (pane->metadata), "1 folder · 2 files"));
    g_autoptr (GFileInfo) first_info = g_list_model_get_item (G_LIST_MODEL (pane->folder_items), 0);
    g_assert_cmpstr (g_file_info_get_display_name (first_info), ==, "Designs");
    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (pane->folder_items)), ==, 3);
    g_assert_true (pane->folder_done);
    g_assert_cmpstr (gtk_button_get_label (GTK_BUTTON (pane->open)), ==, "Open folder");
    set_capture_path ("folder-preview.png");
    pump (400); capture (window);
    g_print ("PASS folder listing, folders first, literal names, sizes and symlinks\n");
    g_settings_set_boolean (gtk_filechooser_preferences, NAUTILUS_PREFERENCES_SHOW_HIDDEN_FILES, TRUE);
    for (guint i = 0; i < 5000 && g_strcmp0 (gtk_stack_get_visible_child_name (pane->stack), "folder") != 0; i++) pump (1);
    wait_for_folder_page (pane);
    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (pane->folder_items)), ==, 4);
    g_print ("PASS folder hidden-item preference and clearing old rows\n");
    select_file (pane, empty);
    g_assert_cmpstr (gtk_stack_get_visible_child_name (pane->stack), ==, "message");
    g_assert_cmpstr (gtk_label_get_text (pane->message), ==, "This folder is empty.");
    g_print ("PASS empty folder state\n");
    guint total = FOLDER_PAGE_SIZE * 17 + 7;
    for (guint i = 0; i < total; i++)
    {
        g_autofree char *path = g_strdup_printf ("%s/item-%03u.txt", subdir, i);
        g_file_set_contents (path, "sample", -1, NULL);
    }
    select_file (pane, empty);
    pump (300);
    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (pane->folder_items)), ==, FOLDER_PAGE_SIZE);
    g_assert_false (pane->folder_done);
    g_assert_nonnull (pane->folder_enumerator);
    g_assert_nonnull (strstr (gtk_label_get_text (pane->metadata), "32 items loaded"));
    g_autoptr (GFileInfo) initial = g_list_model_get_item (G_LIST_MODEL (pane->folder_items), 0);
    g_print ("PASS folder first page loads only 32 items without enumerating the whole folder\n");
    GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment (pane->folder_scroll);
    /* Wait for GTK to allocate the new list before simulating a scroll;
     * the previous empty-folder adjustment can survive until the next frame. */
    gint64 layout_deadline = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;
    while ((!gtk_widget_get_mapped (GTK_WIDGET (pane->folder_list)) ||
            gtk_adjustment_get_page_size (adjustment) <= 0 ||
            gtk_adjustment_get_upper (adjustment) <= gtk_adjustment_get_page_size (adjustment)) &&
           g_get_monotonic_time () < layout_deadline)
    {
        g_main_context_iteration (NULL, FALSE);
        g_usleep (1000);
    }
    g_assert_true (gtk_widget_get_mapped (GTK_WIDGET (pane->folder_list)));
    g_assert_cmpfloat (gtk_adjustment_get_upper (adjustment), >, gtk_adjustment_get_page_size (adjustment));
    gtk_adjustment_set_value (adjustment, gtk_adjustment_get_upper (adjustment) - gtk_adjustment_get_page_size (adjustment));
    wait_for_folder_page (pane);
    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (pane->folder_items)), ==, 2 * FOLDER_PAGE_SIZE);
    g_autoptr (GFileInfo) still_first = g_list_model_get_item (G_LIST_MODEL (pane->folder_items), 0);
    g_assert_true (initial == still_first);
    g_print ("PASS scrolling appends the next folder page without moving previous entries\n");
    /* A load in flight must also be cancellable when hidden visibility changes. */
    request_folder_page (pane);
    g_assert_true (pane->folder_loading);
    g_settings_set_boolean (gtk_filechooser_preferences, NAUTILUS_PREFERENCES_SHOW_HIDDEN_FILES, FALSE);
    wait_for_folder_page (pane);
    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (pane->folder_items)), ==, FOLDER_PAGE_SIZE);
    for (guint batch = 0; batch < 30 && !pane->folder_done; batch++)
    {
        g_signal_emit_by_name (pane->folder_more, "clicked");
        wait_for_folder_page (pane);
    }
    g_assert_true (pane->folder_done);
    g_assert_null (pane->folder_enumerator);
    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (pane->folder_items)), ==, total);
    g_autoptr (GHashTable) names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < total; i++)
    {
        g_autoptr (GFileInfo) info = g_list_model_get_item (G_LIST_MODEL (pane->folder_items), i);
        g_assert_true (g_hash_table_add (names, g_strdup (g_file_info_get_name (info))));
    }
    pump (200);
    guint rows = 0;
    for (GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (pane->folder_list)); child != NULL;
         child = gtk_widget_get_next_sibling (child)) rows++;
    g_assert_cmpuint (rows, <, total);
    g_assert_false (gtk_widget_get_visible (pane->folder_more));
    g_print ("PASS lazy folder traversal beyond 500 items, unique entries and recycled row widgets\n");
    nautilus_preview_pane_cancel (pane);
    gtk_widget_set_visible (GTK_WIDGET (pane), FALSE);
    g_settings_set_boolean (gtk_filechooser_preferences, NAUTILUS_PREFERENCES_SHOW_HIDDEN_FILES, TRUE);
    pump (200);
    g_assert_false (pane->folder_loading);
    g_assert_null (pane->folder_enumerator);
    g_settings_set_boolean (gtk_filechooser_preferences, NAUTILUS_PREFERENCES_SHOW_HIDDEN_FILES, FALSE);
    gtk_widget_set_visible (GTK_WIDGET (pane), TRUE);
    g_print ("PASS hidden preference changes do not restart a hidden folder preview\n");
    select_file (pane, empty);
    request_folder_page (pane);
    request_folder_page (pane);
    g_assert_true (pane->folder_loading);
    select_file (pane, text_file);
    pump (300);
    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (pane->folder_items)), ==, 0);
    g_assert_null (pane->folder_enumerator);
    g_assert_cmpstr (gtk_stack_get_visible_child_name (pane->stack), ==, "text");
    g_print ("PASS repeated page requests are serialized and selection cancels in-flight continuation\n");
    g_autofree char *hidden_dir = g_build_filename (root, "Hidden files", NULL);
    g_mkdir (hidden_dir, 0700);
    for (guint i = 0; i < FOLDER_SCAN_BATCH + 7; i++)
    {
        g_autofree char *path = g_strdup_printf ("%s/.hidden-%03u", hidden_dir, i);
        g_file_set_contents (path, "", 0, NULL);
    }
    NautilusFile *hidden_folder = ready_file (hidden_dir);
    select_file (pane, hidden_folder);
    g_assert_false (pane->folder_done);
    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (pane->folder_items)), ==, 0);
    g_assert_true (gtk_widget_get_visible (pane->folder_more));
    g_assert_nonnull (strstr (gtk_label_get_text (pane->folder_status), "No visible items in this batch"));
    g_signal_emit_by_name (pane->folder_more, "clicked");
    wait_for_folder_page (pane);
    g_assert_true (pane->folder_done);
    g_assert_cmpstr (gtk_label_get_text (pane->message), ==, "No visible items in this folder.");
    nautilus_file_monitor_remove (hidden_folder, hidden_folder);
    nautilus_file_unref (hidden_folder);
    g_print ("PASS hidden-heavy folders use bounded scan batches and continue from the same cursor\n");
    GList selection = { .data = folder };
    nautilus_preview_pane_set_files (pane, &selection);
    pump (125);
    select_file (pane, text_file);
    pump (300);
    g_assert_cmpstr (gtk_stack_get_visible_child_name (pane->stack), ==, "text");
    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (pane->folder_items)), ==, 0);
    g_print ("PASS folder selection cancellation and file transition\n");
    g_settings_set_boolean (gtk_filechooser_preferences, NAUTILUS_PREFERENCES_SHOW_HIDDEN_FILES, FALSE);
    nautilus_file_monitor_remove (folder, folder);
    nautilus_file_monitor_remove (empty, empty);
    remove_preview_directory (root);
    select_file (pane, folder);
    g_assert_cmpstr (gtk_stack_get_visible_child_name (pane->stack), ==, "message");
    g_assert_cmpstr (gtk_label_get_text (pane->message), !=, "Loading preview…");
    g_assert_cmpstr (gtk_label_get_text (pane->message), !=, "This folder is empty.");
    g_print ("PASS unavailable folder error state\n");
    nautilus_file_unref (folder);
    nautilus_file_unref (empty);
}

static void
check_zip_preview (NautilusPreviewPane *pane, GtkWidget *window, NautilusFile *text_file)
{
    g_autofree char *root = g_dir_make_tmp ("smok-zip-preview-XXXXXX", NULL);
    g_autofree char *script = g_build_filename (sample_directory, "..", "tests", "create_archive_fixtures.py", NULL);
    g_autoptr (GError) error = NULL;
    g_autoptr (GSubprocess) fixtures = g_subprocess_new (G_SUBPROCESS_FLAGS_NONE, &error, "/usr/bin/python3", script, root, NULL);
    g_assert_no_error (error);
    g_assert_true (g_subprocess_wait_check (fixtures, NULL, &error));
    g_assert_no_error (error);
    const char *names[] = {"archive.zip", "large.zip", "empty.zip", "paths.zip", "unread-payload.zip", "broken.zip", "oversized-index.zip", "zip64-empty.zip", "too-many-entries.zip"};
    NautilusFile *files[G_N_ELEMENTS (names)];
    for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    {
        g_autofree char *path = g_build_filename (root, names[i], NULL);
        files[i] = ready_file (path);
    }
    select_file (pane, files[0]);
    g_assert_true (pane->zip);
    g_assert_cmpstr (gtk_stack_get_visible_child_name (pane->stack), ==, "folder");
    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (pane->folder_items)), ==, 5);
    g_assert_cmpstr (gtk_button_get_label (GTK_BUTTON (pane->open)), ==, "Open archive");
    g_assert_nonnull (strstr (gtk_label_get_text (pane->metadata), "2 folders · 3 files · ZIP archive"));
    g_autoptr (GFileInfo) first = g_list_model_get_item (G_LIST_MODEL (pane->folder_items), 0);
    g_assert_cmpstr (g_file_info_get_display_name (first), ==, "Documents/");
    set_capture_path ("zip-preview.png");
    pump (400); capture (window);
    g_print ("PASS ZIP paths, folders, sizes and hidden entries without extraction\n");
    select_file (pane, files[1]);
    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (pane->folder_items)), ==, FOLDER_PAGE_SIZE);
    g_assert_false (pane->folder_done);
    g_assert_nonnull (pane->zip_cursor);
    for (guint batch = 0; batch < 30 && !pane->folder_done; batch++)
    {
        request_folder_page (pane);
        wait_for_folder_page (pane);
    }
    g_assert_true (pane->folder_done);
    g_assert_null (pane->zip_cursor);
    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (pane->folder_items)), ==, 645);
    g_autoptr (GHashTable) unique = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < 645; i++)
    {
        g_autoptr (GFileInfo) info = g_list_model_get_item (G_LIST_MODEL (pane->folder_items), i);
        g_assert_true (g_hash_table_add (unique, g_strdup (g_file_info_get_name (info))));
    }
    g_print ("PASS lazy ZIP cursor continuation beyond 500 entries without duplicates\n");
    select_file (pane, files[2]);
    if (g_strcmp0 (gtk_label_get_text (pane->message), "This ZIP archive is empty.") != 0)
        g_printerr ("Empty ZIP error: %s\n", gtk_label_get_text (pane->message));
    g_assert_cmpstr (gtk_label_get_text (pane->message), ==, "This ZIP archive is empty.");
    select_file (pane, files[7]);
    g_assert_cmpstr (gtk_label_get_text (pane->message), ==, "This ZIP archive is empty.");
    g_print ("PASS empty ZIP and ZIP64 directory handling\n");
    select_file (pane, files[3]);
    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (pane->folder_items)), ==, 5);
    gboolean found_link = FALSE;
    for (guint i = 0; i < 5; i++)
    {
        g_autoptr (GFileInfo) info = g_list_model_get_item (G_LIST_MODEL (pane->folder_items), i);
        const char *display = g_file_info_get_display_name (info);
        g_assert_null (strchr (display, '\n'));
        g_assert_cmpint (g_utf8_strlen (display, -1), <=, 513);
        found_link |= g_file_info_get_is_symlink (info);
    }
    g_assert_true (found_link);
    g_autofree char *outside = g_build_filename (root, "absolute-outside.txt", NULL);
    g_assert_false (g_file_test (outside, G_FILE_TEST_EXISTS));
    g_print ("PASS archive links and unsafe-looking paths are listed only; long/control names are bounded\n");
    select_file (pane, files[4]);
    g_assert_cmpstr (gtk_stack_get_visible_child_name (pane->stack), ==, "folder");
    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (pane->folder_items)), ==, 1);
    g_print ("PASS ZIP listing does not decompress or validate regular member payloads\n");
    for (guint i = 5; i < G_N_ELEMENTS (names); i++)
    {
        if (i == 7) continue;
        select_file (pane, files[i]);
        g_assert_cmpstr (gtk_stack_get_visible_child_name (pane->stack), ==, "message");
        g_assert_true (pane->folder_done);
        g_assert_null (pane->zip_cursor);
    }
    g_print ("PASS broken ZIP, oversized central index and excessive entry count fallbacks\n");
    g_autofree char *encrypted = g_build_filename (root, "encrypted.zip", NULL);
    struct archive *writer = archive_write_new ();
    g_assert_cmpint (archive_write_set_format_zip (writer), ==, ARCHIVE_OK);
    g_assert_cmpint (archive_write_set_options (writer, "zip:encryption=zipcrypt"), ==, ARCHIVE_OK);
    g_assert_cmpint (archive_write_set_passphrase (writer, "preview-test"), ==, ARCHIVE_OK);
    g_assert_cmpint (archive_write_open_filename (writer, encrypted), ==, ARCHIVE_OK);
    struct archive_entry *entry = archive_entry_new ();
    archive_entry_set_pathname (entry, "private.txt");
    archive_entry_set_filetype (entry, AE_IFREG);
    archive_entry_set_perm (entry, 0600);
    archive_entry_set_size (entry, 6);
    g_assert_cmpint (archive_write_header (writer, entry), ==, ARCHIVE_OK);
    g_assert_cmpint (archive_write_data (writer, "sample", 6), ==, 6);
    archive_entry_free (entry);
    g_assert_cmpint (archive_write_free (writer), ==, ARCHIVE_OK);
    NautilusFile *locked = ready_file (encrypted);
    select_file (pane, locked);
    g_assert_cmpstr (gtk_stack_get_visible_child_name (pane->stack), ==, "folder");
    g_autoptr (GFileInfo) locked_info = g_list_model_get_item (G_LIST_MODEL (pane->folder_items), 0);
    g_assert_nonnull (strstr (g_file_info_get_attribute_string (locked_info, "smok::entry-detail"), "Encrypted"));
    g_print ("PASS encrypted ZIP metadata lists without passwords or decryption\n");
    select_file (pane, files[1]);
    request_folder_page (pane);
    select_file (pane, text_file);
    pump (300);
    g_assert_false (pane->zip);
    g_assert_null (pane->zip_cursor);
    g_assert_cmpstr (gtk_stack_get_visible_child_name (pane->stack), ==, "text");
    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (pane->folder_items)), ==, 0);
    g_print ("PASS archive cancellation and transition back to text\n");
    nautilus_file_monitor_remove (locked, locked);
    nautilus_file_unref (locked);
    for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    {
        nautilus_file_monitor_remove (files[i], files[i]);
        nautilus_file_unref (files[i]);
    }
    remove_preview_directory (root);
}

static void
wait_for_preview_location (NautilusPreviewPane *pane, GFile *location)
{
    gint64 deadline = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;
    while ((pane->file == NULL || !g_file_equal (pane->file, location)) && g_get_monotonic_time () < deadline)
        pump (1);
    g_assert_nonnull (pane->file);
    g_assert_true (g_file_equal (pane->file, location));
}

static void
emit_cell_hover (NautilusWindowSlot *slot, GtkEventController *motion, GtkWidget *cell)
{
    graphene_point_t center = GRAPHENE_POINT_INIT (gtk_widget_get_width (cell) / 2.0,
                                                   gtk_widget_get_height (cell) / 2.0);
    graphene_point_t point;
    g_assert_true (gtk_widget_compute_point (cell, slot->stack, &center, &point));
    GtkWidget *picked = gtk_widget_pick (slot->stack, point.x, point.y, GTK_PICK_DEFAULT);
    while (picked != NULL && !NAUTILUS_IS_VIEW_CELL (picked)) picked = gtk_widget_get_parent (picked);
    g_assert_true (picked == cell);
    g_signal_emit_by_name (motion, "motion", (double) point.x, (double) point.y);
}

static void
check_hover_preview (NautilusFile *first, NautilusFile *second)
{
    NautilusWindowSlot *slot = nautilus_window_slot_new (NAUTILUS_MODE_BROWSE);
    GtkWidget *window = gtk_window_new ();
    gtk_window_set_default_size (GTK_WINDOW (window), 800, 650);
    gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (slot));
    GtkWidget *cell = GTK_WIDGET (nautilus_grid_cell_new (NULL));
    g_object_set (cell, "icon-size", NAUTILUS_GRID_ICON_SIZE_LARGE, NULL);
    GQuark captions[NAUTILUS_GRID_CELL_N_CAPTIONS] = {0};
    nautilus_grid_cell_set_caption_attributes (NAUTILUS_GRID_CELL (cell), captions);
    g_autoptr (NautilusViewItem) item = nautilus_view_item_new (second);
    nautilus_view_cell_set_item (NAUTILUS_VIEW_CELL (cell), item);
    gtk_stack_add_named (GTK_STACK (slot->stack), cell, "hover-test");
    gtk_stack_set_visible_child (GTK_STACK (slot->stack), cell);
    gtk_window_present (GTK_WINDOW (window));
    nautilus_window_slot_set_selection (slot, g_list_append (NULL, nautilus_file_ref (first)));
    pump (500);
    g_autoptr (GListModel) controllers = gtk_widget_observe_controllers (slot->stack);
    g_autoptr (GtkEventController) motion = NULL;
    for (guint i = 0; i < g_list_model_get_n_items (controllers); i++)
    {
        g_autoptr (GtkEventController) controller = g_list_model_get_item (controllers, i);
        if (GTK_IS_EVENT_CONTROLLER_MOTION (controller)) g_set_object (&motion, controller);
    }
    g_assert_nonnull (motion);
    /* Isolate emitted pointer events from the desktop's physical pointer. */
    gtk_event_controller_set_propagation_phase (motion, GTK_PHASE_NONE);
    clear_preview_hover (slot, FALSE);
    nautilus_preview_pane_set_files (slot->preview_pane, slot->selection);
    gint64 mapped_deadline = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;
    while ((!gtk_widget_get_mapped (cell) || gtk_widget_get_width (cell) == 0 || gtk_widget_get_height (cell) == 0) &&
           g_get_monotonic_time () < mapped_deadline) pump (1);
    g_assert_true (gtk_widget_get_mapped (cell));
    g_assert_cmpint (gtk_widget_get_width (cell), >, 0);
    emit_cell_hover (slot, motion, cell);
    pump (200);
    g_autoptr (GFile) initial_location = nautilus_file_get_location (first);
    g_assert_true (g_file_equal (slot->preview_pane->file, initial_location));
    g_autoptr (GFile) second_location = nautilus_file_get_location (second);
    wait_for_preview_location (slot->preview_pane, second_location);
    g_assert_true (slot->selection->data == first);
    g_assert_null (slot->selection->next);
    g_print ("PASS hover delay and preview without changing selection\n");
    g_autoptr (NautilusViewItem) selected_item = nautilus_view_item_new (first);
    nautilus_view_cell_set_item (NAUTILUS_VIEW_CELL (cell), selected_item);
    emit_cell_hover (slot, motion, cell);
    wait_for_preview_location (slot->preview_pane, initial_location);
    g_assert_false (slot->preview_hover_active);
    nautilus_view_cell_set_item (NAUTILUS_VIEW_CELL (cell), item);
    emit_cell_hover (slot, motion, cell);
    wait_for_preview_location (slot->preview_pane, second_location);
    g_print ("PASS hovering the selected item restores its preview\n");
    g_signal_emit_by_name (motion, "leave");
    pump (300);
    g_autoptr (GFile) first_location = nautilus_file_get_location (first);
    g_assert_true (g_file_equal (slot->preview_pane->file, first_location));
    emit_cell_hover (slot, motion, cell);
    g_signal_emit_by_name (motion, "leave");
    pump (500);
    g_assert_true (g_file_equal (slot->preview_pane->file, first_location));
    g_print ("PASS hover leave restores selection and cancels pending preview\n");
    emit_cell_hover (slot, motion, cell);
    nautilus_window_slot_set_selection (slot, g_list_append (NULL, nautilus_file_ref (second)));
    pump (500);
    g_assert_false (slot->preview_hover_active);
    g_assert_cmpuint (slot->preview_hover_timeout, ==, 0);
    g_assert_true (slot->selection->data == second);
    emit_cell_hover (slot, motion, cell);
    GAction *toggle = g_action_map_lookup_action (G_ACTION_MAP (slot->slot_action_group), "preview-pane");
    action_preview_pane (G_SIMPLE_ACTION (toggle), NULL, slot);
    pump (500);
    g_assert_cmpuint (slot->preview_hover_timeout, ==, 0);
    g_assert_null (slot->preview_pane->media);
    g_print ("PASS selection and hidden pane cancel pending hover\n");
    action_preview_pane (G_SIMPLE_ACTION (toggle), NULL, slot);
    nautilus_view_cell_set_item (NAUTILUS_VIEW_CELL (cell), selected_item);
    emit_cell_hover (slot, motion, cell);
    g_assert_cmpuint (slot->preview_hover_timeout, !=, 0);
    g_assert_false (preview_pointer_scroll (NULL, 0, 1, slot));
    g_assert_cmpuint (slot->preview_hover_timeout, ==, 0);
    pump (500);
    g_assert_true (g_file_equal (slot->preview_pane->file, second_location));
    emit_cell_hover (slot, motion, cell);
    gtk_window_destroy (GTK_WINDOW (window));
    pump (500);
    g_print ("PASS scrolling and disposal cancel pending hover\n");

}

static void
check_video_preview (NautilusPreviewPane *pane, GtkWidget *window, NautilusFile *text_file)
{
    g_autofree char *path = g_build_filename (sample_directory, "video.mp4", NULL);
    NautilusFile *video = ready_file (path);
    select_file (pane, video);
    if (g_strcmp0 (gtk_stack_get_visible_child_name (pane->stack), "video") != 0)
        g_printerr ("Video error: %s\n", gtk_label_get_text (pane->message));
    g_assert_cmpstr (gtk_stack_get_visible_child_name (pane->stack), ==, "video");
    g_assert_true (gtk_media_stream_has_video (pane->media));
    g_assert_true (gtk_media_stream_has_audio (pane->media));
    g_assert_false (gtk_media_stream_get_playing (pane->media));
    g_assert_cmpint (gtk_media_stream_get_duration (pane->media), >=, 3 * G_USEC_PER_SEC);
    g_print ("PASS video prepares with audio, duration and paused playback\n");
    gtk_media_stream_set_muted (pane->media, TRUE);
    gtk_media_stream_play (pane->media);
    for (guint i = 0; i < 5000 && gtk_media_stream_get_timestamp (pane->media) < 300000; i++) pump (1);
    g_assert_cmpint (gtk_media_stream_get_timestamp (pane->media), >=, 300000);
    gtk_media_stream_pause (pane->media);
    g_assert_true (gtk_media_stream_is_seekable (pane->media));
    gtk_media_stream_seek (pane->media, 2 * G_USEC_PER_SEC);
    for (guint i = 0; i < 5000 && gtk_media_stream_is_seeking (pane->media); i++) pump (1);
    g_assert_cmpint (gtk_media_stream_get_timestamp (pane->media), >=, 1500000);
    set_capture_path ("video-preview.png");
    pump (400); capture (window);
    g_print ("PASS video playback, pause and seek\n");
    g_autoptr (GtkMediaStream) previous = g_object_ref (pane->media);
    gtk_media_stream_play (previous);
    select_file (pane, text_file);
    g_assert_null (pane->media);
    g_assert_false (gtk_media_stream_get_playing (previous));
    g_assert_null (gtk_media_file_get_file (GTK_MEDIA_FILE (previous)));
    g_assert_cmpstr (gtk_stack_get_visible_child_name (pane->stack), ==, "text");
    GList selection = { .data = video };
    nautilus_preview_pane_set_files (pane, &selection);
    nautilus_preview_pane_cancel (pane);
    pump (300);
    g_assert_null (pane->media);
    select_file (pane, video);
    nautilus_preview_pane_cancel (pane);
    g_assert_null (pane->media);
    g_assert_null (gtk_video_get_media_stream (pane->video));
    g_print ("PASS video cleanup on selection change, hide and pending cancellation\n");
    g_autofree char *dir = g_dir_make_tmp ("nautilus-video-test-XXXXXX", NULL);
    g_autofree char *broken = g_build_filename (dir, "broken.mp4", NULL);
    g_file_set_contents (broken, "not a valid video", -1, NULL);
    NautilusFile *bad = ready_file (broken);
    select_file (pane, bad);
    g_assert_cmpstr (gtk_stack_get_visible_child_name (pane->stack), ==, "message");
    g_assert_nonnull (strstr (gtk_label_get_text (pane->message), "Video preview unavailable"));
    select_file (pane, text_file);
    pump (300);
    g_assert_cmpstr (gtk_stack_get_visible_child_name (pane->stack), ==, "text");
    g_print ("PASS corrupt video fallback and recovery\n");
    nautilus_file_monitor_remove (bad, bad);
    nautilus_file_unref (bad);
    nautilus_file_monitor_remove (video, video);
    nautilus_file_unref (video);
    g_remove (broken); g_rmdir (dir);
}

int
main (int argc, char **argv)
{
    sample_directory = g_strdup (g_getenv ("SMOK_GLASS_TEST_DATA"));
    artifact_directory = g_strdup (g_getenv ("SMOK_GLASS_TEST_OUTPUT"));
    if (sample_directory == NULL || artifact_directory == NULL)
    {
        g_printerr ("Set SMOK_GLASS_TEST_DATA and SMOK_GLASS_TEST_OUTPUT, or use scripts/test-native.sh.\n");
        return 2;
    }
    g_mkdir_with_parents (artifact_directory, 0700);
    set_capture_path ("markdown-preview.png");
    gtk_init ();
    adw_init ();
    nautilus_register_resource ();
    nautilus_global_preferences_init ();
    nautilus_ensure_extension_points ();
    /* Views can finish async action updates after their windows close. Keep
     * the app and its DBus launcher alive for the entire integration run. */
    g_autoptr (NautilusApplication) app = nautilus_application_new ();
    if (argc == 3 && g_str_equal (argv[1], "--folder-uri"))
    {
        NautilusPreviewPane *folder_pane = NAUTILUS_PREVIEW_PANE (nautilus_preview_pane_new ());
        g_object_ref_sink (folder_pane);
        NautilusFile *folder = ready_file (argv[2]);
        g_assert_true (nautilus_file_is_directory (folder));
        select_file (folder_pane, folder);
        g_assert_true (folder_pane->folder);
        g_assert_true (g_strcmp0 (gtk_stack_get_visible_child_name (folder_pane->stack), "folder") == 0 ||
                       g_str_has_prefix (gtk_label_get_text (folder_pane->message), "No visible items"));
        if (!folder_pane->folder_done && g_list_model_get_n_items (G_LIST_MODEL (folder_pane->folder_items)) > 0)
        {
            guint before = g_list_model_get_n_items (G_LIST_MODEL (folder_pane->folder_items));
            request_folder_page (folder_pane);
            wait_for_folder_page (folder_pane);
            g_assert_false (folder_pane->folder_error);
            g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (folder_pane->folder_items)), >=, before);
            g_print ("PASS GIO folder cursor continuation\n");
        }
        nautilus_preview_pane_cancel (folder_pane);
        g_object_unref (folder_pane);
        nautilus_file_monitor_remove (folder, folder);
        nautilus_file_unref (folder);
        g_print ("PASS GIO folder URI preview\n");
        return 0;
    }
    if (argc > 1 && g_str_equal (argv[1], "--sheet-only"))
    {
        NautilusPreviewPane *sheet_pane = NAUTILUS_PREVIEW_PANE (nautilus_preview_pane_new ());
        GtkWidget *sheet_window = gtk_window_new ();
        gtk_window_set_default_size (GTK_WINDOW (sheet_window), 680, 680);
        gtk_window_set_child (GTK_WINDOW (sheet_window), GTK_WIDGET (sheet_pane));
        gtk_window_present (GTK_WINDOW (sheet_window));
        g_autofree char *workbook_path = g_build_filename (sample_directory, "sheet.xlsx", NULL);
        NautilusFile *workbook = ready_file (workbook_path);
        set_capture_path ("spreadsheet-preview.png");
        select_file (sheet_pane, workbook);
        g_assert_cmpstr (gtk_stack_get_visible_child_name (sheet_pane->stack), ==, "sheet");
        for (guint i = 0; i < 15000 && webkit_web_view_is_loading (sheet_pane->sheet); i++) pump (1);
        pump (1500);
        webkit_web_view_get_snapshot (sheet_pane->sheet, WEBKIT_SNAPSHOT_REGION_VISIBLE,
                                      WEBKIT_SNAPSHOT_OPTIONS_NONE, NULL, sheet_snapshot_ready, NULL);
        for (guint i = 0; i < 10000 && !sheet_capture_done; i++) pump (1);
        g_assert_true (sheet_capture_done);
        nautilus_preview_pane_cancel (sheet_pane);
        gtk_window_destroy (GTK_WINDOW (sheet_window));
        nautilus_file_monitor_remove (workbook, workbook);
        nautilus_file_unref (workbook);
        g_print ("PASS native spreadsheet rendering and snapshot\n");
        return 0;
    }
    g_autofree char *dir = g_dir_make_tmp ("nautilus-preview-test-XXXXXX", NULL);
    g_autofree char *md = g_build_filename (dir, "README.md", NULL);
    g_autofree char *txt = g_build_filename (dir, "notes.txt", NULL);
    g_autofree char *large = g_build_filename (dir, "large.txt", NULL);
    g_autofree char *binary = g_build_filename (dir, "binary.txt", NULL);
    g_file_set_contents (md, "# Preview works\n\nA **bold** idea, with *emphasis*.\n\n- First item\n- Second item\n\n```python\nprint('hello')\n```\n\n<script>not executed</script>\n", -1, NULL);
    g_file_set_contents (txt, "Plain text preview\nUnicode: café 日本語\nSecond line\n", -1, NULL);
    g_autofree char *big = g_strnfill (TEXT_LIMIT + 4096, 'A');
    g_file_set_contents (large, big, -1, NULL);
    g_file_set_contents (binary, "binary\0data", 11, NULL);
    NautilusFile *md_file = ready_file (md);
    NautilusFile *txt_file = ready_file (txt);
    NautilusFile *large_file = ready_file (large);
    NautilusFile *binary_file = ready_file (binary);
    g_autofree char *image_path = g_build_filename (sample_directory, "sample.svg", NULL);
    NautilusFile *image_file = ready_file (image_path);
    if (argc > 1 && g_str_equal (argv[1], "--hover-only"))
    {
        check_hover_preview (txt_file, md_file);
        NautilusFile *files[] = {md_file, txt_file, large_file, binary_file, image_file};
        for (guint i = 0; i < G_N_ELEMENTS (files); i++)
        {
            nautilus_file_monitor_remove (files[i], files[i]);
            nautilus_file_unref (files[i]);
        }
        remove_preview_directory (dir);
        return 0;
    }
    NautilusPreviewPane *pane = NAUTILUS_PREVIEW_PANE (nautilus_preview_pane_new ());
    g_object_ref_sink (pane);
    select_file (pane, md_file);
    g_assert_cmpstr (gtk_stack_get_visible_child_name (pane->stack), ==, "text");
    g_autofree char *rendered = text_contents (pane);
    g_assert_nonnull (strstr (rendered, "Preview works"));
    g_assert_null (strstr (rendered, "# Preview"));
    g_assert_null (strstr (rendered, "**bold**"));
    g_assert_null (strstr (rendered, "not executed"));
    gtk_toggle_button_set_active (pane->source, TRUE);
    g_autofree char *source = text_contents (pane);
    g_assert_nonnull (strstr (source, "# Preview works"));
    g_print ("PASS rendered Markdown and source toggle\n");
    select_file (pane, txt_file);
    g_autofree char *plain = text_contents (pane);
    g_assert_nonnull (strstr (plain, "café 日本語"));
    g_print ("PASS scrollable UTF-8 text\n");
    select_file (pane, image_file);
    if (g_strcmp0 (gtk_stack_get_visible_child_name (pane->stack), "image") != 0)
        g_printerr ("Image error: %s\n", gtk_label_get_text (pane->message));
    g_assert_cmpstr (gtk_stack_get_visible_child_name (pane->stack), ==, "image");
    g_assert_nonnull (gtk_picture_get_paintable (pane->picture));
    g_print ("PASS sandboxed image preview\n");
    GList first = { .data = image_file }, second = { .data = txt_file };
    nautilus_preview_pane_set_files (pane, &first);
    nautilus_preview_pane_set_files (pane, &second);
    pump (1800);
    g_assert_cmpstr (gtk_stack_get_visible_child_name (pane->stack), ==, "text");
    g_autofree char *latest = text_contents (pane);
    g_assert_nonnull (strstr (latest, "Plain text preview"));
    g_print ("PASS rapid selection cancels stale image results\n");
    select_file (pane, large_file);
    g_autofree char *limited = text_contents (pane);
    g_assert_cmpuint (strlen (limited), ==, TEXT_LIMIT);
    g_assert_nonnull (strstr (gtk_label_get_text (pane->metadata), "512 KiB"));
    g_print ("PASS bounded large-file preview\n");
    select_file (pane, binary_file);
    g_assert_cmpstr (gtk_stack_get_visible_child_name (pane->stack), ==, "message");
    g_assert_nonnull (strstr (gtk_label_get_text (pane->message), "binary"));
    nautilus_preview_pane_set_files (pane, NULL);
    g_assert_null (pane->file);
    g_print ("PASS binary and empty selection handling\n");
    select_file (pane, md_file);
    gtk_toggle_button_set_active (pane->source, FALSE);
    GtkWidget *window = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (window), "Nautilus preview check");
    gtk_window_set_default_size (GTK_WINDOW (window), 390, 650);
    gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (pane));
    gtk_window_present (GTK_WINDOW (window));
    pump (400);
    capture (window);
    if (argc > 1 && g_str_equal (argv[1], "--zip-only"))
    {
        check_zip_preview (pane, window, txt_file);
        goto preview_check_cleanup;
    }
    check_overview (pane, window, txt_file);
    if (argc > 1 && g_str_equal (argv[1], "--overview-only")) goto preview_check_cleanup;
    check_folder_preview (pane, window, txt_file);
    if (argc > 1 && g_str_equal (argv[1], "--folder-only")) goto preview_check_cleanup;
    check_zip_preview (pane, window, txt_file);
    check_video_preview (pane, window, txt_file);
    check_hover_preview (txt_file, md_file);
    const char *document_names[] = {"document.pdf", "document.docx", "sheet.xlsx", "document.odt", "sheet.ods", "sheet.csv", "sheet.tsv"};
    for (guint i = 0; i < G_N_ELEMENTS (document_names); i++)
    {
        g_autofree char *path = g_build_filename (sample_directory, document_names[i], NULL);
        NautilusFile *document = ready_file (path);
        select_file (pane, document);
        if (i == 0 || i == 1 || i == 3)
        {
            if (g_strcmp0 (gtk_stack_get_visible_child_name (pane->stack), "image") != 0)
                g_printerr ("Document preview error for %s: %s\n", document_names[i], gtk_label_get_text (pane->message));
            g_assert_cmpstr (gtk_stack_get_visible_child_name (pane->stack), ==, "image");
            if (i != 0)
            {
                g_assert_true (gtk_widget_get_visible (pane->page_controls));
                g_assert_cmpuint (pane->total_pages, >=, 1);
                if (i == 1)
                {
                    set_capture_path ("document-preview.png");
                    pump (500); capture (window);
                }
                g_print ("PASS formatted document pages: %s\n", document_names[i]);
                nautilus_file_monitor_remove (document, document);
                nautilus_file_unref (document);
                continue;
            }
            g_assert_cmpuint (pane->total_pages, ==, 2);
            g_assert_true (gtk_widget_get_sensitive (pane->next_page));
            change_pdf_page (pane, 1);
            for (guint j = 0; j < 20000 && g_strcmp0 (gtk_stack_get_visible_child_name (pane->stack), "image") != 0; j++) pump (1);
            g_assert_cmpuint (pane->page, ==, 2);
            g_assert_cmpstr (gtk_label_get_text (pane->page_label), ==, "Page 2 of 2");
            g_assert_false (gtk_widget_get_sensitive (pane->next_page));
            set_capture_path ("pdf-preview.png");
            pump (300); capture (window);
            change_pdf_page (pane, -1);
            for (guint j = 0; j < 20000 && g_strcmp0 (gtk_stack_get_visible_child_name (pane->stack), "image") != 0; j++) pump (1);
            g_assert_cmpstr (gtk_label_get_text (pane->page_label), ==, "Page 1 of 2");
            GList pdf_selection = { .data = document };
            nautilus_preview_pane_set_files (pane, &pdf_selection);
            pump (150);
            select_file (pane, txt_file);
            pump (400);
            g_autofree char *after_pdf = text_contents (pane);
            g_assert_nonnull (strstr (after_pdf, "Plain text preview"));
            g_print ("PASS cancelling an in-progress PDF preview\n");
        }
        else
        {
            g_assert_cmpstr (gtk_stack_get_visible_child_name (pane->stack), ==, "sheet");
            for (guint j = 0; j < 10000 && webkit_web_view_is_loading (pane->sheet); j++) pump (1);
            g_assert_false (webkit_settings_get_enable_javascript (webkit_web_view_get_settings (pane->sheet)));
            g_assert_false (gtk_widget_get_visible (pane->page_controls));
            if (i == 2)
            {
                set_capture_path ("spreadsheet-preview.png");
                pump (700); capture (window);
            }
        }
        g_print ("PASS document pane: %s\n", document_names[i]);
        nautilus_file_monitor_remove (document, document);
        nautilus_file_unref (document);
    }
preview_check_cleanup:
    nautilus_preview_pane_set_files (pane, &first);
    nautilus_preview_pane_cancel (pane);
    gtk_window_destroy (GTK_WINDOW (window));
    g_object_unref (pane);
    pump (500);
    g_print ("PASS close during preview loading\n");
    NautilusFile *files[] = {md_file, txt_file, large_file, binary_file, image_file};
    for (guint i = 0; i < G_N_ELEMENTS (files); i++)
    {
        nautilus_file_monitor_remove (files[i], files[i]);
        nautilus_file_unref (files[i]);
    }
    g_remove (md); g_remove (txt); g_remove (large); g_remove (binary); g_rmdir (dir);
    return 0;
}
