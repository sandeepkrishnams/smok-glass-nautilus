/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"
#include "nautilus-preview-pane.h"
#include "nautilus-global-preferences.h"
#include "nautilus-folder-usage.h"
#include <adwaita.h>
#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cmark.h>
#include <glycin.h>
#include <glycin-gtk4.h>
#include <string.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <unistd.h>
#include <webkit/webkit.h>

#define TEXT_LIMIT (512 * 1024)
#define FOLDER_PAGE_SIZE 32
#define FOLDER_SCAN_BATCH 256
#define ZIP_INDEX_LIMIT (16 * 1024 * 1024)
#define ZIP_ENTRY_LIMIT 100000

typedef struct
{
    struct archive *reader;
    int fd;
    guint entries_read;
} ZipCursor;

struct _NautilusPreviewPane
{
    GtkBox parent_instance;
    GtkLabel *name;
    GtkLabel *metadata;
    GtkLabel *message;
    GtkDrawingArea *storage_chart;
    GtkLabel *storage_label, *storage_percent, *overview_counts;
    GtkWidget *overview_rows[NAUTILUS_USAGE_SEGMENTS], *overview_refresh;
    GtkLabel *overview_names[NAUTILUS_USAGE_SEGMENTS], *overview_values[NAUTILUS_USAGE_SEGMENTS];
    GFile *overview_location;
    GListModel *overview_model;
    gulong overview_changed;
    guint overview_source;
    gboolean overview_active, overview_loading;
    GTask *overview_task;
    NautilusUsageResult usage_result;
    gint64 usage_checked;
    GtkStack *stack;
    GtkPicture *picture;
    GtkVideo *video;
    GtkMediaStream *media;
    GtkTextView *text;
    GtkToggleButton *source;
    GtkWidget *open;
    GtkWidget *text_scroll;
    GtkWidget *page_controls;
    GtkWidget *previous_page;
    GtkWidget *next_page;
    GtkLabel *page_label;
    WebKitWebView *sheet;
    GtkListView *folder_list;
    GListStore *folder_items;
    GtkScrolledWindow *folder_scroll;
    GtkWidget *folder_more;
    GtkLabel *folder_status;
    GFileEnumerator *folder_enumerator;
    ZipCursor *zip_cursor;
    gboolean zip;
    gboolean folder_loading, folder_done, folder_error;
    guint folder_count;
    gboolean folder_show_hidden;
    gboolean folder;
    const char *document_kind;
    guint page, total_pages;
    GFile *file;
    GCancellable *cancellable;
    char *contents;
    gboolean markdown;
    guint64 serial;
    guint load_source;
};

G_DEFINE_FINAL_TYPE (NautilusPreviewPane, nautilus_preview_pane, GTK_TYPE_BOX)

typedef struct
{
    GFile *file;
    guint64 serial;
    gboolean image;
    gboolean started;
    char *text;
    GlyFrame *frame;
    guint width, height;
    gboolean truncated;
    char *document_kind;
    char *temporary_directory;
    guint page, total_pages;
    gboolean folder, show_hidden;
    GPtrArray *entries;
    GFileEnumerator *enumerator;
    gboolean exhausted;
    ZipCursor *zip_cursor;
    gboolean zip;
} PreviewLoad;

/* Folder usage is scanned on one worker, with fixed-size progress snapshots. */
static const char *usage_colors[NAUTILUS_USAGE_SEGMENTS] = {
    "#70a5ed", "#a88beb", "#71c6a0", "#efb568", "#e889a5", "#70c4d4", "#9ca3b5", "#d4c26a"
};
static void show_overview (NautilusPreviewPane *self);

static void
overview_draw (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
    NautilusPreviewPane *self = data;
    double radius = MIN (width, height) * 0.36;
    GdkRGBA foreground;
    gtk_widget_get_color (GTK_WIDGET (area), &foreground);
    cairo_set_line_width (cr, 18);
    cairo_set_source_rgba (cr, foreground.red, foreground.green, foreground.blue, 0.15);
    cairo_arc (cr, width / 2.0, height / 2.0, radius, 0, 2 * G_PI);
    cairo_stroke (cr);
    double start = -G_PI / 2;
    NautilusUsageResult *result = &self->usage_result;
    for (guint i = 0; result->bytes > 0 && i < result->n_segments; i++)
    {
        if (result->segments[i].bytes == 0) continue;
        double end = start + 2 * G_PI * ((double) result->segments[i].bytes / result->bytes);
        GdkRGBA color;
        gdk_rgba_parse (&color, usage_colors[i]);
        gdk_cairo_set_source_rgba (cr, &color);
        cairo_arc (cr, width / 2.0, height / 2.0, radius, start, end);
        cairo_stroke (cr);
        start = end;
    }
}

static void
overview_render (NautilusPreviewPane *self)
{
    NautilusUsageResult *result = &self->usage_result;
    g_autofree char *size = g_format_size (result->bytes);
    gtk_label_set_text (self->storage_percent, size);
    gtk_label_set_text (self->storage_label,
        result->unreadable ? "Unable to read this folder." :
        result->running ? "Scanning… sizes and shares so far" :
        result->cancelled ? "Scan stopped — totals are partial." :
        result->partial ? "Partial totals — some paths were unreadable or a scan limit was reached." :
        result->files + result->folders == 0 ? "This folder is empty." : "Total file sizes in this folder");
    guint64 items = result->files + result->folders;
    g_autofree char *counts = g_strdup_printf ("%" G_GUINT64_FORMAT " items · %" G_GUINT64_FORMAT
        " files · %" G_GUINT64_FORMAT " folders", items, result->files, result->folders);
    gtk_label_set_text (self->overview_counts, counts);
    for (guint i = 0; i < NAUTILUS_USAGE_SEGMENTS; i++)
    {
        gtk_widget_set_visible (self->overview_rows[i], i < result->n_segments);
        if (i >= result->n_segments) continue;
        NautilusUsageSegment *segment = &result->segments[i];
        gtk_label_set_text (self->overview_names[i], segment->name);
        gtk_widget_set_tooltip_text (GTK_WIDGET (self->overview_names[i]), segment->name);
        g_autofree char *part_size = g_format_size (segment->bytes);
        g_autofree char *value = g_strdup_printf ("%s · %.1f%%", part_size,
            result->bytes > 0 ? 100.0 * segment->bytes / result->bytes : 0.0);
        gtk_label_set_text (self->overview_values[i], value);
    }
    gtk_button_set_label (GTK_BUTTON (self->overview_refresh), result->running ? "Stop scan" : "Rescan folder");
    gtk_widget_queue_draw (GTK_WIDGET (self->storage_chart));
}

static gboolean
overview_progress (gpointer data)
{
    NautilusPreviewPane *self = data;
    if (self->overview_task != NULL)
    {
        NautilusFolderUsage *usage = g_task_get_task_data (self->overview_task);
        nautilus_folder_usage_snapshot (usage, &self->usage_result);
        overview_render (self);
    }
    return G_SOURCE_CONTINUE;
}

static void
overview_scan_thread (GTask *task, gpointer object, gpointer data, GCancellable *cancellable)
{
    nautilus_folder_usage_scan (data, cancellable);
    g_task_return_boolean (task, TRUE);
}

static void
overview_scan_ready (GObject *object, GAsyncResult *result, gpointer data)
{
    NautilusPreviewPane *self = NAUTILUS_PREVIEW_PANE (object);
    GTask *task = G_TASK (result);
    g_task_propagate_boolean (task, NULL);
    if (task != self->overview_task || !self->overview_active) return;
    g_clear_handle_id (&self->overview_source, g_source_remove);
    nautilus_folder_usage_snapshot (g_task_get_task_data (task), &self->usage_result);
    self->usage_checked = g_get_monotonic_time ();
    overview_render (self);
    g_clear_object (&self->overview_task);
}

static gboolean
overview_start (gpointer data)
{
    NautilusPreviewPane *self = data;
    self->overview_source = 0;
    if (!self->overview_active || self->overview_location == NULL || self->overview_loading) return G_SOURCE_REMOVE;
    g_clear_object (&self->cancellable);
    self->cancellable = g_cancellable_new ();
    memset (&self->usage_result, 0, sizeof self->usage_result);
    self->usage_result.running = TRUE;
    self->usage_checked = 0;
    overview_render (self);
    self->overview_task = g_task_new (self, self->cancellable, overview_scan_ready, NULL);
    g_task_set_priority (self->overview_task, G_PRIORITY_LOW);
    g_task_set_task_data (self->overview_task, nautilus_folder_usage_new (self->overview_location),
                         (GDestroyNotify) nautilus_folder_usage_free);
    g_task_run_in_thread (self->overview_task, overview_scan_thread);
    self->overview_source = g_timeout_add (150, overview_progress, self);
    return G_SOURCE_REMOVE;
}

static void
overview_refresh_clicked (GtkButton *button, NautilusPreviewPane *self)
{
    if (!self->overview_active) return;
    if (self->overview_task != NULL)
    {
        nautilus_folder_usage_snapshot (g_task_get_task_data (self->overview_task), &self->usage_result);
        nautilus_preview_pane_cancel (self);
        self->overview_active = TRUE;
        self->usage_result.running = FALSE;
        self->usage_result.cancelled = self->usage_result.partial = TRUE;
        self->usage_checked = g_get_monotonic_time ();
        overview_render (self);
        return;
    }
    g_clear_handle_id (&self->overview_source, g_source_remove);
    if (!self->overview_loading) overview_start (self);
}

static void
overview_items_changed (GListModel *model, guint position, guint removed, guint added, NautilusPreviewPane *self)
{
    /* Do not restart recursive work on every file-list/filter/sort update. */
    self->usage_checked = 0;
    if (self->overview_active && self->overview_task == NULL && self->overview_source == 0 && !self->overview_loading)
        gtk_label_set_text (self->storage_label, "Folder view changed. Rescan to update totals.");
}

void
nautilus_preview_pane_set_context (NautilusPreviewPane *self, GFile *location, GListModel *model, gboolean loading)
{
    gboolean same = (location == NULL && self->overview_location == NULL) ||
        (location != NULL && self->overview_location != NULL && g_file_equal (location, self->overview_location));
    if (!same || model != self->overview_model)
    {
        nautilus_preview_pane_cancel (self);
        if (self->overview_changed) g_signal_handler_disconnect (self->overview_model, self->overview_changed);
        self->overview_changed = 0;
        g_set_object (&self->overview_location, location);
        g_set_object (&self->overview_model, model);
        if (model != NULL)
            self->overview_changed = g_signal_connect (model, "items-changed", G_CALLBACK (overview_items_changed), self);
        self->usage_checked = 0;
        memset (&self->usage_result, 0, sizeof self->usage_result);
    }
    self->overview_loading = loading;
}

static void
show_overview (NautilusPreviewPane *self)
{
    self->overview_active = TRUE;
    gtk_widget_set_visible (self->open, FALSE);
    gtk_stack_set_visible_child_name (self->stack, "overview");
    gtk_label_set_text (self->name, "Folder usage");
    g_autofree char *name = g_file_get_basename (self->overview_location);
    gtk_label_set_text (self->metadata, name != NULL ? name : "Current folder");
    overview_render (self);
    if (self->usage_checked == 0 && !self->usage_result.running &&
        self->usage_result.files + self->usage_result.folders == 0)
    {
        gtk_label_set_text (self->storage_percent, "—");
        gtk_label_set_text (self->overview_counts, "");
    }
    gtk_widget_set_sensitive (self->overview_refresh, !self->overview_loading);
    if (self->overview_loading)
        gtk_label_set_text (self->storage_label, "Waiting for this folder to load…");
    else if (self->usage_checked != 0 && g_get_monotonic_time () - self->usage_checked < 30 * G_USEC_PER_SEC)
        return;
    else if (!g_file_is_native (self->overview_location))
    {
        gtk_label_set_text (self->storage_label, "Calculate this network folder’s size when you need it.");
        gtk_button_set_label (GTK_BUTTON (self->overview_refresh), "Calculate folder usage");
    }
    else
    {
        gtk_label_set_text (self->storage_label, "Preparing folder scan…");
        /* Avoid starting recursive I/O while a pointer passes over the view. */
        self->overview_source = g_timeout_add (450, overview_start, self);
    }
}

static gboolean
is_page_document (const char *kind)
{
    return g_strcmp0 (kind, "pdf") == 0 || g_strcmp0 (kind, "docx") == 0 ||
           g_strcmp0 (kind, "odt") == 0 || g_strcmp0 (kind, "doc") == 0 || g_strcmp0 (kind, "rtf") == 0;
}

static gboolean
is_spreadsheet (const char *kind)
{
    return g_strcmp0 (kind, "xlsx") == 0 || g_strcmp0 (kind, "xls") == 0 ||
           g_strcmp0 (kind, "ods") == 0 || g_strcmp0 (kind, "csv") == 0 || g_strcmp0 (kind, "tsv") == 0;
}

static void
remove_preview_directory (const char *path)
{
    g_autoptr (GDir) directory = g_dir_open (path, 0, NULL);
    if (directory != NULL)
    {
        const char *name;
        while ((name = g_dir_read_name (directory)) != NULL)
        {
            g_autofree char *child = g_build_filename (path, name, NULL);
            if (!g_file_test (child, G_FILE_TEST_IS_SYMLINK) && g_file_test (child, G_FILE_TEST_IS_DIR))
                remove_preview_directory (child);
            else g_unlink (child);
        }
    }
    g_rmdir (path);
}

static void
release_folder_enumerator (GFileEnumerator **enumerator)
{
    if (*enumerator == NULL) return;
    /* Network backends may block while closing. Never close on the GTK thread. */
    g_file_enumerator_close_async (*enumerator, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
    g_clear_object (enumerator);
}

static void
zip_close_thread (GTask *task, gpointer object, gpointer data, GCancellable *cancel)
{
    ZipCursor *cursor = data;
    if (cursor->reader != NULL) archive_read_free (cursor->reader);
    if (cursor->fd >= 0) close (cursor->fd);
    g_free (cursor);
    g_task_return_boolean (task, TRUE);
}

static void
release_zip_cursor (ZipCursor **cursor)
{
    if (*cursor == NULL) return;
    g_autoptr (GTask) task = g_task_new (NULL, NULL, NULL, NULL);
    g_task_set_task_data (task, g_steal_pointer (cursor), NULL);
    g_task_run_in_thread (task, zip_close_thread);
}

static void
preview_load_free (gpointer data)
{
    PreviewLoad *load = data;
    g_clear_object (&load->file);
    g_clear_object (&load->frame);
    g_free (load->text);
    g_free (load->document_kind);
    g_clear_pointer (&load->entries, g_ptr_array_unref);
    release_folder_enumerator (&load->enumerator);
    release_zip_cursor (&load->zip_cursor);
    if (load->temporary_directory != NULL)
    {
        remove_preview_directory (load->temporary_directory);
        g_free (load->temporary_directory);
    }
    g_free (load);
}

static void
preview_child_setup (gpointer data)
{
    /* The helper and its PDF renderer share a private process group. */
    (void) setpgid (0, 0);
}

static void
cancel_document_process (GCancellable *cancel, gpointer data)
{
    GSubprocess *process = G_SUBPROCESS (data);
    const char *identifier = g_subprocess_get_identifier (process);
    if (identifier != NULL)
    {
        gint64 pid = g_ascii_strtoll (identifier, NULL, 10);
        if (pid > 1) (void) kill ((pid_t) -pid, SIGKILL);
    }
}

static gboolean
load_document (PreviewLoad *load, GCancellable *cancel, GError **error)
{
    g_autofree char *path = g_file_get_path (load->file);
    g_autofree char *page = g_strdup_printf ("%u", MAX (1, load->page));
    gboolean pdf = is_page_document (load->document_kind);
    load->temporary_directory = g_dir_make_tmp ("nautilus-document-preview-XXXXXX", error);
    if (load->temporary_directory == NULL) return FALSE;
    g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_set_child_setup (launcher, preview_child_setup, NULL, NULL);
    g_subprocess_launcher_setenv (launcher, "LC_ALL", "C.UTF-8", TRUE);
    const char *argv[] = {"/usr/bin/python3",
        NAUTILUS_DATADIR "/smok-glass/preview-documents.py", load->document_kind,
        path, page, load->temporary_directory, NULL};
    g_autoptr (GSubprocess) process = g_subprocess_launcher_spawnv (launcher, argv, error);
    if (process == NULL) return FALSE;
    gulong handler = g_cancellable_connect (cancel, G_CALLBACK (cancel_document_process), process, NULL);
    g_autofree char *output = NULL;
    g_autofree char *diagnostic = NULL;
    gboolean communicated = g_subprocess_communicate_utf8 (process, NULL, cancel, &output, &diagnostic, error);
    g_cancellable_disconnect (cancel, handler);
    if (!communicated)
    {
        g_subprocess_force_exit (process);
        return FALSE;
    }
    if (!g_subprocess_get_successful (process))
    {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
                     diagnostic != NULL && *diagnostic != '\0' ? g_strstrip (diagnostic) : "Document preview unavailable");
        return FALSE;
    }
    if (pdf) load->total_pages = CLAMP (g_ascii_strtoull (output, NULL, 10), 1, 100000);
    else load->text = g_steal_pointer (&output);
    return TRUE;
}

static gint
compare_folder_entries (gconstpointer a, gconstpointer b)
{
    GFileInfo *first = *(GFileInfo * const *) a;
    GFileInfo *second = *(GFileInfo * const *) b;
    gboolean first_directory = g_file_info_get_file_type (first) == G_FILE_TYPE_DIRECTORY;
    gboolean second_directory = g_file_info_get_file_type (second) == G_FILE_TYPE_DIRECTORY;
    if (first_directory != second_directory) return first_directory ? -1 : 1;
    g_autofree char *first_key = g_utf8_collate_key_for_filename (g_file_info_get_display_name (first), -1);
    g_autofree char *second_key = g_utf8_collate_key_for_filename (g_file_info_get_display_name (second), -1);
    return strcmp (first_key, second_key);
}

static gboolean
load_folder (PreviewLoad *load, GCancellable *cancel, GError **error)
{
    load->entries = g_ptr_array_new_with_free_func (g_object_unref);
    if (load->enumerator == NULL)
        load->enumerator = g_file_enumerate_children (
            load->file, "standard::name,standard::display-name,standard::type,standard::size,"
                        "standard::icon,standard::is-hidden,standard::is-backup,standard::is-symlink",
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, error);
    if (load->enumerator == NULL) return FALSE;
    /* Bound both visible results and filtering work; a hidden-heavy directory
     * must not force us to scan the entire directory before returning. */
    for (guint scanned = 0; scanned < FOLDER_SCAN_BATCH && load->entries->len < FOLDER_PAGE_SIZE; scanned++)
    {
        g_autoptr (GFileInfo) info = g_file_enumerator_next_file (load->enumerator, cancel, error);
        if (info == NULL)
        {
            load->exhausted = *error == NULL;
            return *error == NULL;
        }
        const char *name = g_file_info_get_name (info);
        gboolean hidden = g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN)
                          ? g_file_info_get_is_hidden (info) : g_str_has_prefix (name, ".");
        gboolean backup = g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP)
                          ? g_file_info_get_is_backup (info) : g_str_has_suffix (name, "~");
        if (!load->show_hidden && (hidden || backup)) continue;
        g_ptr_array_add (load->entries, g_steal_pointer (&info));
    }
    return TRUE;
}

static guint64
zip_uint (const guint8 *bytes, guint size)
{
    guint64 value = 0;
    for (guint i = 0; i < size; i++) value |= (guint64) bytes[i] << (8 * i);
    return value;
}

/* The seekable reader indexes the central directory. Bound that index before
 * handing it to libarchive; member payload sizes are irrelevant to listing. */
static gboolean
zip_check_index (int fd, goffset size, gboolean *empty, GError **error)
{
    *empty = FALSE;
    gsize length = MIN (size, 65557);
    g_autofree guint8 *tail = g_malloc (MAX (length, 1));
    if (length < 22 || pread (fd, tail, length, size - length) != (ssize_t) length) goto invalid;
    for (gssize i = length - 22; i >= 0; i--)
    {
        if (memcmp (tail + i, "PK\005\006", 4) != 0 || i + 22 + zip_uint (tail + i + 20, 2) != length) continue;
        guint64 entries = zip_uint (tail + i + 10, 2);
        guint64 index_size = zip_uint (tail + i + 12, 4);
        guint64 index_offset = zip_uint (tail + i + 16, 4);
        if (zip_uint (tail + i + 4, 2) != 0 || zip_uint (tail + i + 6, 2) != 0)
        {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Multi-volume ZIP previews are unavailable. Open the archive in its application.");
            return FALSE;
        }
        if (entries == 0xffff || index_size == 0xffffffff || index_offset == 0xffffffff)
        {
            guint8 locator[20], record[56];
            goffset eocd = size - length + i;
            if (eocd < 20 || pread (fd, locator, 20, eocd - 20) != 20 || memcmp (locator, "PK\006\007", 4) != 0) goto invalid;
            guint64 offset = zip_uint (locator + 8, 8);
            if (size < 56 || offset > (guint64) size - 56 || pread (fd, record, 56, offset) != 56 || memcmp (record, "PK\006\006", 4) != 0) goto invalid;
            if (zip_uint (locator + 4, 4) != 0 || zip_uint (locator + 16, 4) != 1 ||
                zip_uint (record + 16, 4) != 0 || zip_uint (record + 20, 4) != 0) goto invalid;
            entries = zip_uint (record + 32, 8);
            index_size = zip_uint (record + 40, 8);
            index_offset = zip_uint (record + 48, 8);
        }
        if (entries > ZIP_ENTRY_LIMIT || index_size > ZIP_INDEX_LIMIT)
        {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "This ZIP index is too large for an inline preview (100,000 entries or 16 MiB). Open the archive in its application.");
            return FALSE;
        }
        if (index_offset > (guint64) size || index_size > (guint64) size - index_offset) goto invalid;
        *empty = entries == 0 && index_size == 0;
        return TRUE;
    }
invalid:
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "ZIP preview unavailable. The archive is damaged, incomplete, or has an unsupported directory.");
    return FALSE;
}

static gboolean
load_zip (PreviewLoad *load, GCancellable *cancel, GError **error)
{
    load->entries = g_ptr_array_new_with_free_func (g_object_unref);
    if (load->zip_cursor == NULL)
    {
        load->zip_cursor = g_new0 (ZipCursor, 1);
        ZipCursor *cursor = load->zip_cursor;
        g_autofree char *path = g_file_get_path (load->file);
        cursor->fd = open (path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        struct stat statbuf;
        if (cursor->fd < 0 || fstat (cursor->fd, &statbuf) != 0 || !S_ISREG (statbuf.st_mode))
        {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot read this ZIP file. Open it in its application.");
            goto failed;
        }
        gboolean empty;
        if (!zip_check_index (cursor->fd, statbuf.st_size, &empty, error)) goto failed;
        if (empty) { load->exhausted = TRUE; return TRUE; }
        if (g_cancellable_set_error_if_cancelled (cancel, error)) goto failed;
        cursor->reader = archive_read_new ();
        archive_read_support_format_zip_seekable (cursor->reader);
        if (archive_read_open_fd (cursor->reader, cursor->fd, 65536) != ARCHIVE_OK) goto archive_error;
    }
    for (guint i = 0; i < FOLDER_PAGE_SIZE; i++)
    {
        if (g_cancellable_set_error_if_cancelled (cancel, error)) goto failed;
        struct archive_entry *entry;
        int result = archive_read_next_header (load->zip_cursor->reader, &entry);
        if (result == ARCHIVE_EOF) { load->exhausted = TRUE; return TRUE; }
        if ((result != ARCHIVE_OK && result != ARCHIVE_WARN) || entry == NULL) goto archive_error;
        if (++load->zip_cursor->entries_read > ZIP_ENTRY_LIMIT)
        {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "ZIP preview entry limit reached. Open the archive to see more.");
            goto failed;
        }
        const char *path = archive_entry_pathname_utf8 (entry);
        if (path == NULL) path = archive_entry_pathname (entry);
        g_autofree char *valid = g_utf8_make_valid (path != NULL ? path : "(unnamed entry)", -1);
        for (char *c = valid; *c; c++) if ((guchar) *c < 32 || *c == 127) *c = ' ';
        g_autofree char *short_name = g_utf8_substring (valid, 0, MIN (g_utf8_strlen (valid, -1), 512));
        g_autofree char *display = g_utf8_strlen (valid, -1) > 512 ? g_strconcat (short_name, "…", NULL) : g_strdup (valid);
        g_autoptr (GFileInfo) info = g_file_info_new ();
        gboolean directory = archive_entry_filetype (entry) == AE_IFDIR || g_str_has_suffix (valid, "/");
        gboolean link = archive_entry_filetype (entry) == AE_IFLNK;
        g_file_info_set_name (info, valid);
        g_file_info_set_display_name (info, display);
        g_file_info_set_file_type (info, directory ? G_FILE_TYPE_DIRECTORY : link ? G_FILE_TYPE_SYMBOLIC_LINK : G_FILE_TYPE_REGULAR);
        g_file_info_set_is_symlink (info, link);
        g_autofree char *content_type = directory ? g_strdup ("inode/directory") : g_content_type_guess (valid, NULL, 0, NULL);
        g_autoptr (GIcon) icon = g_content_type_get_symbolic_icon (content_type);
        if (icon != NULL) g_file_info_set_icon (info, icon);
        g_file_info_set_size (info, MAX (0, archive_entry_size (entry)));
        if (archive_entry_is_encrypted (entry))
        {
            g_autofree char *size = g_format_size (MAX (0, archive_entry_size (entry)));
            g_autofree char *detail = g_strdup_printf ("Encrypted · %s", size);
            g_file_info_set_attribute_string (info, "smok::entry-detail", detail);
        }
        g_ptr_array_add (load->entries, g_steal_pointer (&info));
        /* No archive_read_data/extract call: the seekable ZIP backend skips
         * payloads and obtains names/sizes from the bounded central index. */
    }
    return TRUE;
archive_error:
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "ZIP preview unavailable: %s",
                 archive_error_string (load->zip_cursor->reader) != NULL ? archive_error_string (load->zip_cursor->reader) : "Unsupported or damaged archive");
failed:
    load->exhausted = TRUE;
    return FALSE;
}

/* File I/O and sandboxed image decoding run off the GTK main thread. */
static void
preview_load_thread (GTask *task, gpointer object, gpointer data, GCancellable *cancel)
{
    PreviewLoad *load = data;
    g_autoptr (GError) error = NULL;
    if (g_task_return_error_if_cancelled (task)) return;
    if (load->folder)
    {
        if (!(load->zip ? load_zip (load, cancel, &error) : load_folder (load, cancel, &error)))
            g_task_return_error (task, g_steal_pointer (&error));
        else
        {
            g_ptr_array_sort (load->entries, compare_folder_entries);
            g_task_return_boolean (task, TRUE);
        }
        return;
    }
    if (load->document_kind != NULL)
    {
        if (!load_document (load, cancel, &error))
        {
            g_task_return_error (task, g_steal_pointer (&error));
            return;
        }
        if (!load->image)
        {
            g_task_return_boolean (task, TRUE);
            return;
        }
    }
    g_autoptr (GFile) image_file = NULL;
    if (load->temporary_directory != NULL)
    {
        g_autofree char *png = g_build_filename (load->temporary_directory, "page.png", NULL);
        image_file = g_file_new_for_path (png);
    }
    g_autoptr (GFileInputStream) stream = g_file_read (image_file != NULL ? image_file : load->file, cancel, &error);
    if (stream == NULL)
    {
        g_task_return_error (task, g_steal_pointer (&error));
        return;
    }
    if (load->image)
    {
        g_autoptr (GlyLoader) loader = gly_loader_new_for_stream (G_INPUT_STREAM (stream));
        g_autoptr (GlyImage) image = gly_loader_load (loader, &error);
        if (image == NULL)
        {
            g_task_return_error (task, g_steal_pointer (&error));
            return;
        }
        if (g_task_return_error_if_cancelled (task)) return;
        load->width = gly_image_get_width (image);
        load->height = gly_image_get_height (image);
        g_autoptr (GlyFrameRequest) request = gly_frame_request_new ();
        gly_frame_request_set_scale (request, 1200, 1200);
        load->frame = gly_image_get_specific_frame (image, request, &error);
        if (load->frame == NULL)
        {
            g_task_return_error (task, g_steal_pointer (&error));
            return;
        }
    }
    else
    {
        g_autofree char *bytes = g_malloc (TEXT_LIMIT + 2);
        gsize count = 0;
        if (!g_input_stream_read_all (G_INPUT_STREAM (stream), bytes, TEXT_LIMIT + 1, &count, cancel, &error))
        {
            g_task_return_error (task, g_steal_pointer (&error));
            return;
        }
        load->truncated = count > TEXT_LIMIT;
        count = MIN (count, TEXT_LIMIT);
        if (memchr (bytes, '\0', count) != NULL)
        {
            g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                     "This file contains binary data. Open it in its application.");
            return;
        }
        load->text = g_utf8_make_valid (bytes, count);
    }
    g_task_return_boolean (task, TRUE);
}

static void
append_text (GtkTextBuffer *buffer, const char *text)
{
    GtkTextIter end;
    gtk_text_buffer_get_end_iter (buffer, &end);
    gtk_text_buffer_insert (buffer, &end, text, -1);
}

/* CommonMark is parsed locally. HTML is never executed; remote resources are never fetched. */
static void
render_node (GtkTextBuffer *buffer, cmark_node *node, guint depth)
{
    if (depth > 128)
    {
        return;
    }
    cmark_node_type type = cmark_node_get_type (node);
    int start = gtk_text_buffer_get_char_count (buffer);
    const char *tag = NULL;
    if (type == CMARK_NODE_TEXT || type == CMARK_NODE_CODE || type == CMARK_NODE_CODE_BLOCK)
    {
        const char *literal = cmark_node_get_literal (node);
        if (literal != NULL) append_text (buffer, literal);
    }
    else if (type == CMARK_NODE_SOFTBREAK || type == CMARK_NODE_LINEBREAK)
    {
        append_text (buffer, "\n");
    }
    else if (type == CMARK_NODE_THEMATIC_BREAK)
    {
        append_text (buffer, "────────────\n");
    }
    else if (type == CMARK_NODE_HTML_BLOCK || type == CMARK_NODE_HTML_INLINE)
    {
        /* Raw HTML is intentionally omitted from a read-only document preview. */
    }
    else
    {
        if (type == CMARK_NODE_ITEM) append_text (buffer, "•  ");
        if (type == CMARK_NODE_IMAGE) append_text (buffer, "[Image: ");
        for (cmark_node *child = cmark_node_first_child (node); child != NULL;
             child = cmark_node_next (child))
        {
            render_node (buffer, child, depth + 1);
        }
        if (type == CMARK_NODE_IMAGE) append_text (buffer, "]");
    }
    if (type == CMARK_NODE_HEADING) tag = cmark_node_get_heading_level (node) == 1 ? "heading1" : "heading2";
    else if (type == CMARK_NODE_STRONG) tag = "strong";
    else if (type == CMARK_NODE_EMPH) tag = "emphasis";
    else if (type == CMARK_NODE_CODE || type == CMARK_NODE_CODE_BLOCK) tag = "code";
    else if (type == CMARK_NODE_LINK) tag = "link";
    else if (type == CMARK_NODE_BLOCK_QUOTE) tag = "quote";
    if (tag != NULL)
    {
        GtkTextIter begin, end;
        gtk_text_buffer_get_iter_at_offset (buffer, &begin, start);
        gtk_text_buffer_get_end_iter (buffer, &end);
        gtk_text_buffer_apply_tag_by_name (buffer, tag, &begin, &end);
    }
    if (type == CMARK_NODE_PARAGRAPH || type == CMARK_NODE_HEADING ||
        type == CMARK_NODE_CODE_BLOCK || type == CMARK_NODE_LIST || type == CMARK_NODE_BLOCK_QUOTE)
    {
        append_text (buffer, "\n\n");
    }
}

static void
render_text (NautilusPreviewPane *self)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer (self->text);
    gtk_text_buffer_set_text (buffer, "", 0);
    if (self->contents == NULL) return;
    gboolean rendered = self->markdown && !gtk_toggle_button_get_active (self->source);
    gboolean document_text = g_strcmp0 (self->document_kind, "docx") == 0 || g_strcmp0 (self->document_kind, "odt") == 0;
    gtk_text_view_set_monospace (self->text, !rendered && !document_text);
    if (rendered)
    {
        cmark_node *document = cmark_parse_document (self->contents, strlen (self->contents), CMARK_OPT_DEFAULT);
        if (document != NULL)
        {
            render_node (buffer, document, 0);
            cmark_node_free (document);
        }
    }
    else
    {
        gtk_text_buffer_set_text (buffer, self->contents, -1);
    }
    GtkTextIter start;
    gtk_text_buffer_get_start_iter (buffer, &start);
    gtk_text_buffer_place_cursor (buffer, &start);
    gtk_text_view_scroll_to_iter (self->text, &start, 0, FALSE, 0, 0);
}

static void request_folder_page (NautilusPreviewPane *self);

static void
folder_row_setup (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_top (row, 8);
    gtk_widget_set_margin_bottom (row, 8);
    gtk_widget_set_margin_start (row, 8);
    gtk_widget_set_margin_end (row, 8);
    GtkWidget *image = gtk_image_new ();
    gtk_image_set_pixel_size (GTK_IMAGE (image), 24);
    gtk_box_append (GTK_BOX (row), image);
    GtkWidget *name = gtk_label_new (NULL);
    gtk_label_set_xalign (GTK_LABEL (name), 0);
    gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_single_line_mode (GTK_LABEL (name), TRUE);
    gtk_widget_set_hexpand (name, TRUE);
    gtk_box_append (GTK_BOX (row), name);
    GtkWidget *detail = gtk_label_new (NULL);
    gtk_widget_add_css_class (detail, "dim-label");
    gtk_widget_add_css_class (detail, "caption");
    gtk_box_append (GTK_BOX (row), detail);
    gtk_list_item_set_selectable (item, FALSE);
    gtk_list_item_set_activatable (item, FALSE);
    gtk_list_item_set_child (item, row);
}

static void
folder_row_bind (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
    GFileInfo *info = gtk_list_item_get_item (item);
    GtkWidget *row = gtk_list_item_get_child (item);
    GtkWidget *image = gtk_widget_get_first_child (row);
    GtkWidget *name = gtk_widget_get_next_sibling (image);
    GtkWidget *detail = gtk_widget_get_next_sibling (name);
    gboolean directory = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;
    gboolean symlink = g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK) &&
                       g_file_info_get_is_symlink (info);
    GIcon *icon = g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_ICON) ? g_file_info_get_icon (info) : NULL;
    if (symlink) gtk_image_set_from_icon_name (GTK_IMAGE (image), "insert-link-symbolic");
    else if (icon != NULL) gtk_image_set_from_gicon (GTK_IMAGE (image), icon);
    else gtk_image_set_from_icon_name (GTK_IMAGE (image), directory ? "folder-symbolic" : "text-x-generic-symbolic");
    gtk_label_set_text (GTK_LABEL (name), g_file_info_get_display_name (info));
    gtk_widget_set_tooltip_text (name, g_file_info_get_display_name (info));
    g_autofree char *size = directory ? g_strdup ("Folder") : symlink ? g_strdup ("Link") :
        g_file_info_get_file_type (info) == G_FILE_TYPE_REGULAR ? g_format_size (g_file_info_get_size (info)) : g_strdup ("File");
    const char *entry_detail = g_file_info_get_attribute_string (info, "smok::entry-detail");
    gtk_label_set_text (GTK_LABEL (detail), entry_detail != NULL ? entry_detail : size);
}

static void
folder_batch_loaded (NautilusPreviewPane *self, PreviewLoad *load, const GError *error)
{
    /* The cursor belongs to exactly one worker or the pane, never both. */
    self->folder_enumerator = g_steal_pointer (&load->enumerator);
    self->zip_cursor = g_steal_pointer (&load->zip_cursor);
    self->folder_done = load->exhausted;
    self->folder_error = error != NULL;
    if (load->entries != NULL && load->entries->len > 0)
    {
        for (guint i = 0; i < load->entries->len; i++)
            self->folder_count += g_file_info_get_file_type (g_ptr_array_index (load->entries, i)) == G_FILE_TYPE_DIRECTORY;
        g_list_store_splice (self->folder_items, g_list_model_get_n_items (G_LIST_MODEL (self->folder_items)),
                             0, load->entries->pdata, load->entries->len);
    }
    guint count = g_list_model_get_n_items (G_LIST_MODEL (self->folder_items));
    g_autofree char *summary = g_strdup_printf ("%u %s · %u %s%s", self->folder_count,
        self->folder_count == 1 ? "folder" : "folders", count - self->folder_count,
        count - self->folder_count == 1 ? "file" : "files",
        self->zip ? " · ZIP archive" : self->folder_show_hidden ? " · including hidden items" : " · hidden items excluded");
    g_autofree char *metadata = self->folder_done ? g_strdup (summary) :
        g_strdup_printf ("%u items loaded · scroll for more\n%s", count, summary);
    gtk_label_set_text (self->metadata, metadata);
    gtk_label_set_text (self->folder_status, error != NULL ? error->message :
        count == 0 && !self->folder_done ? "No visible items in this batch. Load more to continue." : "");
    gtk_widget_set_visible (GTK_WIDGET (self->folder_status), error != NULL || (count == 0 && !self->folder_done));
    gtk_button_set_label (GTK_BUTTON (self->folder_more), error != NULL ? "Retry loading" : "Load more");
    gtk_widget_set_visible (self->folder_more, !self->folder_done);
    gtk_widget_set_sensitive (self->folder_more, TRUE);
    if (count == 0 && (self->folder_done || error != NULL))
    {
        gtk_label_set_text (self->message, error != NULL ? error->message :
                            self->zip ? "This ZIP archive is empty." : self->folder_show_hidden ? "This folder is empty." : "No visible items in this folder.");
        gtk_stack_set_visible_child_name (self->stack, "message");
    }
    else gtk_stack_set_visible_child_name (self->stack, "folder");
    if (self->folder_done)
    {
        release_folder_enumerator (&self->folder_enumerator);
        release_zip_cursor (&self->zip_cursor);
    }
    /* Appending can synchronously change adjustments: do not start a second
     * worker until the complete batch has been applied. */
    self->folder_loading = FALSE;
}

static void
folder_scroll_changed (GtkAdjustment *adjustment, NautilusPreviewPane *self)
{
    double page = gtk_adjustment_get_page_size (adjustment);
    double upper = gtk_adjustment_get_upper (adjustment);
    double value = gtk_adjustment_get_value (adjustment);
    if (!self->folder || self->folder_loading || self->folder_done || self->folder_error ||
        self->folder_items == NULL || g_list_model_get_n_items (G_LIST_MODEL (self->folder_items)) == 0 ||
        !gtk_widget_get_mapped (GTK_WIDGET (self->folder_list)) || page <= 0) return;
    if ((value > 0 && upper - value - page < 120) || upper <= page)
        request_folder_page (self);
}

static gboolean
folder_scroll_input (GtkEventControllerScroll *controller, double dx, double dy,
                     NautilusPreviewPane *self)
{
    /* At the bottom the adjustment may not change (for example after a batch
     * containing only hidden entries). Another downward scroll still advances. */
    if (dy > 0) folder_scroll_changed (gtk_scrolled_window_get_vadjustment (self->folder_scroll), self);
    return GDK_EVENT_PROPAGATE;
}

static void
folder_more_clicked (GtkButton *button, NautilusPreviewPane *self)
{
    request_folder_page (self);
}

static void
preview_loaded (GObject *object, GAsyncResult *result, gpointer data)
{
    NautilusPreviewPane *self = NAUTILUS_PREVIEW_PANE (object);
    GTask *task = G_TASK (result);
    PreviewLoad *load = g_task_get_task_data (task);
    g_autoptr (GError) error = NULL;
    gboolean success = g_task_propagate_boolean (task, &error);
    if (load->serial != self->serial) return;
    if (load->folder && !success)
    {
        if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) folder_batch_loaded (self, load, error);
        return;
    }
    if (!success)
    {
        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) return;
        gtk_label_set_text (self->message, error != NULL ? error->message : "Preview unavailable");
        gtk_widget_set_sensitive (self->previous_page, self->page > 1);
        gtk_widget_set_sensitive (self->next_page, self->page < self->total_pages);
        gtk_stack_set_visible_child_name (self->stack, "message");
        return;
    }
    if (load->folder)
    {
        folder_batch_loaded (self, load, NULL);
    }
    else if (load->image)
    {
        g_autoptr (GdkTexture) texture = gly_gtk_frame_get_texture (load->frame);
        gtk_picture_set_paintable (self->picture, GDK_PAINTABLE (texture));
        gtk_stack_set_visible_child_name (self->stack, "image");
        gboolean pdf = is_page_document (load->document_kind);
        g_autofree char *size = pdf ? g_strdup ("Read-only document · formatted page preview")
                                    : g_strdup_printf ("%u × %u pixels", load->width, load->height);
        gtk_label_set_text (self->metadata, size);
        if (pdf)
        {
            self->total_pages = load->total_pages;
            g_autofree char *label = g_strdup_printf ("Page %u of %u", self->page, self->total_pages);
            gtk_label_set_text (self->page_label, label);
            gtk_widget_set_sensitive (self->previous_page, self->page > 1);
            gtk_widget_set_sensitive (self->next_page, self->page < self->total_pages);
        }
    }
    else if (is_spreadsheet (load->document_kind))
    {
        webkit_web_view_load_html (self->sheet, load->text, "about:blank");
        gtk_stack_set_visible_child_name (self->stack, "sheet");
    }
    else
    {
        self->contents = g_strdup (load->text);
        render_text (self);
        gtk_stack_set_visible_child_name (self->stack, "text");
        if (load->truncated) gtk_label_set_text (self->metadata, "Showing the first 512 KiB · open file for the rest");
    }
}

/* Only the selected stream can update the pane; detached streams have no callbacks. */
static void
video_state_changed (GtkMediaStream *stream, GParamSpec *pspec, NautilusPreviewPane *self)
{
    if (stream != self->media) return;
    const GError *error = gtk_media_stream_get_error (stream);
    if (error != NULL)
    {
        g_autofree char *message = g_strdup_printf (
            "Video preview unavailable: %s\nTry Open in application, or install the required GStreamer codec.", error->message);
        gtk_label_set_text (self->message, message);
        gtk_stack_set_visible_child_name (self->stack, "message");
        gtk_media_stream_pause (stream);
    }
    else if (gtk_media_stream_is_prepared (stream))
    {
        if (!gtk_media_stream_has_video (stream))
        {
            gtk_label_set_text (self->message, "This file does not contain a video track. Use Open in application.");
            gtk_stack_set_visible_child_name (self->stack, "message");
            return;
        }
        gint64 seconds = gtk_media_stream_get_duration (stream) / G_USEC_PER_SEC;
        g_autofree char *metadata = seconds > 0 ?
            g_strdup_printf ("Video · %" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT " · press Play to preview", seconds / 60, seconds % 60) :
            g_strdup ("Video · press Play to preview");
        gtk_label_set_text (self->metadata, metadata);
        gtk_stack_set_visible_child_name (self->stack, "video");
    }
}

static gboolean
start_video_preview (gpointer data)
{
    NautilusPreviewPane *self = data;
    self->load_source = 0;
    self->media = gtk_media_file_new ();
    g_signal_connect (self->media, "notify::prepared", G_CALLBACK (video_state_changed), self);
    g_signal_connect (self->media, "notify::error", G_CALLBACK (video_state_changed), self);
    gtk_video_set_media_stream (self->video, self->media);
    gtk_media_file_set_file (GTK_MEDIA_FILE (self->media), self->file);
    video_state_changed (self->media, NULL, self);
    return G_SOURCE_REMOVE;
}

void
nautilus_preview_pane_cancel (NautilusPreviewPane *self)
{
    self->serial++;
    self->overview_active = FALSE;
    g_clear_handle_id (&self->overview_source, g_source_remove);
    g_clear_object (&self->overview_task);
    release_folder_enumerator (&self->folder_enumerator);
    release_zip_cursor (&self->zip_cursor);
    self->folder_loading = FALSE;
    self->folder_done = TRUE;
    if (self->media != NULL)
    {
        g_signal_handlers_disconnect_by_data (self->media, self);
        gtk_media_stream_pause (self->media);
        gtk_video_set_media_stream (self->video, NULL);
        gtk_media_file_clear (GTK_MEDIA_FILE (self->media));
        g_clear_object (&self->media);
    }
    if (self->cancellable != NULL) g_cancellable_cancel (self->cancellable);
    g_clear_object (&self->cancellable);
    if (self->load_source != 0)
    {
        guint source = self->load_source;
        self->load_source = 0;
        g_source_remove (source);
    }
}

static gboolean
start_preview (gpointer data)
{
    GTask *task = G_TASK (data);
    NautilusPreviewPane *self = g_task_get_source_object (task);
    self->load_source = 0;
    PreviewLoad *load = g_task_get_task_data (task);
    load->started = TRUE;
    g_task_run_in_thread (task, preview_load_thread);
    return G_SOURCE_REMOVE;
}

static void
release_pending_preview (gpointer data)
{
    GTask *task = G_TASK (data);
    PreviewLoad *load = g_task_get_task_data (task);
    if (!load->started)
    {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Preview superseded");
    }
    g_object_unref (task);
}

static void
queue_preview (NautilusPreviewPane *self, gboolean image)
{
    gtk_label_set_text (self->message, "Loading preview…");
    gtk_stack_set_visible_child_name (self->stack, "message");
    self->cancellable = g_cancellable_new ();
    PreviewLoad *load = g_new0 (PreviewLoad, 1);
    load->file = g_object_ref (self->file);
    load->serial = self->serial;
    load->image = image;
    load->document_kind = g_strdup (self->document_kind);
    load->page = self->page;
    load->folder = self->folder;
    load->zip = self->zip;
    load->show_hidden = self->folder_show_hidden;
    if (self->folder)
    {
        self->folder_loading = TRUE;
        self->folder_done = FALSE;
        self->folder_error = FALSE;
        gtk_widget_set_sensitive (self->folder_more, FALSE);
    }
    g_autoptr (GTask) task = g_task_new (self, self->cancellable, preview_loaded, NULL);
    g_task_set_task_data (task, load, preview_load_free);
    self->load_source = g_timeout_add_full (G_PRIORITY_DEFAULT, 120, start_preview,
                                          g_object_ref (task), release_pending_preview);
}

static void
request_folder_page (NautilusPreviewPane *self)
{
    if (!self->folder || self->folder_loading || self->folder_done || self->file == NULL || self->cancellable == NULL) return;
    self->folder_loading = TRUE;
    self->folder_error = FALSE;
    gtk_button_set_label (GTK_BUTTON (self->folder_more), "Loading…");
    gtk_widget_set_sensitive (self->folder_more, FALSE);
    PreviewLoad *load = g_new0 (PreviewLoad, 1);
    load->file = g_object_ref (self->file);
    load->serial = self->serial;
    load->folder = TRUE;
    load->zip = self->zip;
    load->show_hidden = self->folder_show_hidden;
    load->enumerator = g_steal_pointer (&self->folder_enumerator);
    load->zip_cursor = g_steal_pointer (&self->zip_cursor);
    g_autoptr (GTask) task = g_task_new (self, self->cancellable, preview_loaded, NULL);
    g_task_set_task_data (task, load, preview_load_free);
    g_task_run_in_thread (task, preview_load_thread);
}

static void
reset_folder_preview (NautilusPreviewPane *self)
{
    self->folder_count = 0;
    self->folder_error = FALSE;
    self->folder_show_hidden = g_settings_get_boolean (gtk_filechooser_preferences, NAUTILUS_PREFERENCES_SHOW_HIDDEN_FILES);
    g_list_store_remove_all (self->folder_items);
    gtk_adjustment_set_value (gtk_scrolled_window_get_vadjustment (self->folder_scroll), 0);
    gtk_widget_set_visible (self->folder_more, FALSE);
    gtk_widget_set_visible (GTK_WIDGET (self->folder_status), FALSE);
}

static void
change_pdf_page (NautilusPreviewPane *self, int delta)
{
    if (self->file == NULL || !is_page_document (self->document_kind)) return;
    int page = CLAMP ((int) self->page + delta, 1, (int) self->total_pages);
    if (page == (int) self->page) return;
    nautilus_preview_pane_cancel (self);
    self->page = page;
    gtk_widget_set_sensitive (self->previous_page, FALSE);
    gtk_widget_set_sensitive (self->next_page, FALSE);
    queue_preview (self, TRUE);
}

static void
folder_visibility_changed (GSettings *settings, const char *key, NautilusPreviewPane *self)
{
    if (!self->folder || self->zip || self->file == NULL || !gtk_widget_get_visible (GTK_WIDGET (self))) return;
    nautilus_preview_pane_cancel (self);
    reset_folder_preview (self);
    queue_preview (self, FALSE);
}

static void
previous_page_clicked (GtkButton *button, NautilusPreviewPane *self)
{
    change_pdf_page (self, -1);
}

static void
next_page_clicked (GtkButton *button, NautilusPreviewPane *self)
{
    change_pdf_page (self, 1);
}

static void
open_file (GtkButton *button, NautilusPreviewPane *self)
{
    if (self->file == NULL) return;
    g_autoptr (GtkFileLauncher) launcher = gtk_file_launcher_new (self->file);
    GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
    gtk_file_launcher_launch (launcher, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL,
                              NULL, NULL, NULL);
}

static void
source_toggled (GtkToggleButton *button, NautilusPreviewPane *self)
{
    render_text (self);
}

void
nautilus_preview_pane_set_files (NautilusPreviewPane *self, GList *files)
{
    nautilus_preview_pane_cancel (self);
    g_clear_object (&self->file);
    g_clear_pointer (&self->contents, g_free);
    gtk_text_buffer_set_text (gtk_text_view_get_buffer (self->text), "", 0);
    gtk_picture_set_paintable (self->picture, NULL);
    reset_folder_preview (self);
    gtk_widget_set_visible (GTK_WIDGET (self->source), FALSE);
    gtk_widget_set_visible (self->page_controls, FALSE);
    self->document_kind = NULL;
    self->folder = FALSE;
    self->zip = FALSE;
    gtk_widget_set_visible (self->open, TRUE);
    gtk_button_set_label (GTK_BUTTON (self->open), "Open in application");
    self->page = 1;
    self->total_pages = 0;
    gtk_text_view_set_wrap_mode (self->text, GTK_WRAP_WORD_CHAR);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self->text_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_sensitive (self->open, FALSE);
    gtk_stack_set_visible_child_name (self->stack, "message");
    gtk_label_set_text (self->metadata, "");
    if (files == NULL || files->next != NULL)
    {
        if (files == NULL && self->overview_location != NULL)
        {
            show_overview (self);
            return;
        }
        guint count = g_list_length (files);
        g_autofree char *title = count == 0 ? g_strdup ("Preview") : g_strdup_printf ("%u items selected", count);
        gtk_label_set_text (self->name, title);
        gtk_label_set_text (self->message, "Select a file or folder to preview it here.");
        return;
    }
    NautilusFile *file = files->data;
    self->file = nautilus_file_get_location (file);
    const char *mime = nautilus_file_get_mime_type (file);
    const char *name = nautilus_file_get_display_name (file);
    gtk_label_set_text (self->name, name);
    g_autofree char *size = g_format_size (nautilus_file_get_size (file));
    g_autofree char *description = mime != NULL ? g_content_type_get_description (mime) : g_strdup ("File");
    g_autofree char *metadata = g_strdup_printf ("%s · %s", description, size);
    gtk_label_set_text (self->metadata, metadata);
    gtk_widget_set_sensitive (self->open, TRUE);
    if (nautilus_file_is_directory (file))
    {
        self->folder = TRUE;
        gtk_label_set_text (self->metadata, "Folder · loading contents…");
        gtk_button_set_label (GTK_BUTTON (self->open), "Open folder");
        queue_preview (self, FALSE);
        return;
    }
    if (!g_file_is_native (self->file))
    {
        gtk_label_set_text (self->message, "Open this remote file to view its contents.");
        return;
    }
    g_autofree char *archive_name = g_ascii_strdown (name, -1);
    gboolean zipped_document = g_str_has_suffix (archive_name, ".docx") || g_str_has_suffix (archive_name, ".xlsx") ||
                               g_str_has_suffix (archive_name, ".odt") || g_str_has_suffix (archive_name, ".ods");
    if (g_str_has_suffix (archive_name, ".zip") || g_str_has_suffix (archive_name, ".zipx") ||
        (!zipped_document && (g_strcmp0 (mime, "application/zip") == 0 || g_strcmp0 (mime, "application/x-zip-compressed") == 0)))
    {
        self->folder = TRUE;
        self->zip = TRUE;
        gtk_button_set_label (GTK_BUTTON (self->open), "Open archive");
        gtk_label_set_text (self->metadata, "ZIP archive · reading contents…");
        queue_preview (self, FALSE);
        return;
    }
    if (mime != NULL && g_str_has_prefix (mime, "video/"))
    {
        gtk_label_set_text (self->message, "Loading preview…");
        self->load_source = g_timeout_add_full (G_PRIORITY_DEFAULT, 120, start_video_preview,
                                               g_object_ref (self), g_object_unref);
        return;
    }
    gboolean image = mime != NULL && g_str_has_prefix (mime, "image/");
    g_autofree char *lower_name = g_ascii_strdown (name, -1);
    const char *kinds[] = {"pdf", "docx", "xlsx", "odt", "ods", "csv", "tsv", "doc", "xls", "rtf"};
    const char *types[] = {"application/pdf", "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
                          "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
                          "application/vnd.oasis.opendocument.text", "application/vnd.oasis.opendocument.spreadsheet",
                          "text/csv", "text/tab-separated-values", "application/msword", "application/vnd.ms-excel", "application/rtf"};
    for (guint i = 0; i < G_N_ELEMENTS (kinds); i++)
    {
        g_autofree char *suffix = g_strconcat (".", kinds[i], NULL);
        if (g_str_has_suffix (lower_name, suffix) || g_strcmp0 (mime, types[i]) == 0) self->document_kind = kinds[i];
    }
    if (mime != NULL && g_str_equal (mime, "application/pdf")) self->document_kind = "pdf";
    gboolean pdf = is_page_document (self->document_kind);
    image = image || pdf;
    gboolean text = mime != NULL &&
        (g_str_has_prefix (mime, "text/") || g_content_type_is_a (mime, "text/plain") ||
         g_str_equal (mime, "application/json") || g_str_equal (mime, "application/xml") ||
         g_str_equal (mime, "application/javascript") || g_str_equal (mime, "application/x-yaml"));
    if ((image || self->document_kind != NULL) && nautilus_file_get_size (file) > 100 * 1024 * 1024)
    {
        gtk_label_set_text (self->message, "This file is larger than 100 MiB. Open it in its application.");
        return;
    }
    if (!image && !text && self->document_kind == NULL)
    {
        gtk_label_set_text (self->message, "No inline preview for this file type. Use Open to view it.");
        return;
    }
    self->markdown = g_str_has_suffix (lower_name, ".md") || g_str_has_suffix (lower_name, ".markdown") ||
                     (mime != NULL && g_str_equal (mime, "text/markdown"));
    gtk_widget_set_visible (GTK_WIDGET (self->source), self->markdown);
    gboolean tabular = g_strcmp0 (self->document_kind, "xlsx") == 0 ||
                        g_strcmp0 (self->document_kind, "ods") == 0 ||
                        g_strcmp0 (self->document_kind, "csv") == 0 ||
                        g_strcmp0 (self->document_kind, "tsv") == 0;
    if (tabular)
    {
        gtk_text_view_set_wrap_mode (self->text, GTK_WRAP_NONE);
        gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self->text_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    }
    if (pdf)
    {
        gtk_widget_set_visible (self->page_controls, TRUE);
        gtk_label_set_text (self->page_label, "Loading…");
        gtk_widget_set_sensitive (self->previous_page, FALSE);
        gtk_widget_set_sensitive (self->next_page, FALSE);
    }
    queue_preview (self, image);
}

static void
nautilus_preview_pane_dispose (GObject *object)
{
    NautilusPreviewPane *self = NAUTILUS_PREVIEW_PANE (object);
    nautilus_preview_pane_cancel (self);
    self->folder = FALSE;
    if (self->overview_changed) g_signal_handler_disconnect (self->overview_model, self->overview_changed);
    self->overview_changed = 0;
    g_clear_object (&self->overview_model);
    g_clear_object (&self->overview_location);
    g_clear_object (&self->folder_items);
    g_clear_object (&self->file);
    g_clear_pointer (&self->contents, g_free);
    G_OBJECT_CLASS (nautilus_preview_pane_parent_class)->dispose (object);
}

static void
nautilus_preview_pane_class_init (NautilusPreviewPaneClass *klass)
{
    G_OBJECT_CLASS (klass)->dispose = nautilus_preview_pane_dispose;
}

static void
nautilus_preview_pane_init (NautilusPreviewPane *self)
{
    gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing (GTK_BOX (self), 10);
    gtk_widget_add_css_class (GTK_WIDGET (self), "smoked-preview-pane");
    gtk_widget_set_size_request (GTK_WIDGET (self), 260, -1);
    gtk_widget_set_margin_start (GTK_WIDGET (self), 16);
    gtk_widget_set_margin_end (GTK_WIDGET (self), 16);
    gtk_widget_set_margin_top (GTK_WIDGET (self), 12);
    gtk_widget_set_margin_bottom (GTK_WIDGET (self), 12);

    GtkWidget *toolbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *heading = gtk_label_new ("Preview");
    gtk_widget_add_css_class (heading, "heading");
    gtk_widget_set_hexpand (heading, TRUE);
    gtk_label_set_xalign (GTK_LABEL (heading), 0);
    gtk_box_append (GTK_BOX (toolbar), heading);
    GtkWidget *close = gtk_button_new_from_icon_name ("window-close-symbolic");
    gtk_widget_add_css_class (close, "flat");
    gtk_widget_set_tooltip_text (close, "Hide preview (Ctrl+Shift+P)");
    gtk_actionable_set_action_name (GTK_ACTIONABLE (close), "slot.preview-pane");
    gtk_box_append (GTK_BOX (toolbar), close);
    gtk_box_append (GTK_BOX (self), toolbar);
    self->name = GTK_LABEL (gtk_label_new ("Preview"));
    gtk_label_set_wrap (self->name, TRUE);
    gtk_label_set_selectable (self->name, TRUE);
    gtk_label_set_xalign (self->name, 0);
    gtk_widget_add_css_class (GTK_WIDGET (self->name), "heading");
    gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->name));
    self->metadata = GTK_LABEL (gtk_label_new (""));
    gtk_label_set_wrap (self->metadata, TRUE);
    gtk_label_set_xalign (self->metadata, 0);
    gtk_widget_add_css_class (GTK_WIDGET (self->metadata), "dim-label");
    gtk_widget_add_css_class (GTK_WIDGET (self->metadata), "caption");
    gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->metadata));
    self->source = GTK_TOGGLE_BUTTON (gtk_toggle_button_new_with_label ("Show Markdown source"));
    gtk_widget_set_visible (GTK_WIDGET (self->source), FALSE);
    g_signal_connect (self->source, "toggled", G_CALLBACK (source_toggled), self);
    gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->source));

    self->stack = GTK_STACK (gtk_stack_new ());
    gtk_widget_set_vexpand (GTK_WIDGET (self->stack), TRUE);
    gtk_stack_set_hhomogeneous (self->stack, FALSE);
    gtk_stack_set_vhomogeneous (self->stack, FALSE);
    self->message = GTK_LABEL (gtk_label_new ("Select a file or folder to preview it here."));
    gtk_label_set_wrap (self->message, TRUE);
    gtk_widget_add_css_class (GTK_WIDGET (self->message), "dim-label");
    gtk_stack_add_named (self->stack, GTK_WIDGET (self->message), "message");
    GtkWidget *overview = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    self->storage_chart = GTK_DRAWING_AREA (gtk_drawing_area_new ());
    gtk_drawing_area_set_content_height (self->storage_chart, 180);
    gtk_drawing_area_set_draw_func (self->storage_chart, overview_draw, self, NULL);
    /* The adjacent label provides the complete accessible storage values. */
    gtk_accessible_update_property (GTK_ACCESSIBLE (self->storage_chart),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Storage used by this folder’s contents", -1);
    GtkWidget *storage_overlay = gtk_overlay_new ();
    gtk_overlay_set_child (GTK_OVERLAY (storage_overlay), GTK_WIDGET (self->storage_chart));
    self->storage_percent = GTK_LABEL (gtk_label_new ("…"));
    gtk_label_set_justify (self->storage_percent, GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class (GTK_WIDGET (self->storage_percent), "title-2");
    gtk_widget_set_halign (GTK_WIDGET (self->storage_percent), GTK_ALIGN_CENTER);
    gtk_widget_set_valign (GTK_WIDGET (self->storage_percent), GTK_ALIGN_CENTER);
    gtk_overlay_add_overlay (GTK_OVERLAY (storage_overlay), GTK_WIDGET (self->storage_percent));
    gtk_box_append (GTK_BOX (overview), storage_overlay);
    self->storage_label = GTK_LABEL (gtk_label_new ("Preparing folder scan…"));
    gtk_label_set_wrap (self->storage_label, TRUE);
    gtk_box_append (GTK_BOX (overview), GTK_WIDGET (self->storage_label));
    self->overview_counts = GTK_LABEL (gtk_label_new (""));
    gtk_label_set_wrap (self->overview_counts, TRUE);
    gtk_widget_add_css_class (GTK_WIDGET (self->overview_counts), "heading");
    gtk_box_append (GTK_BOX (overview), GTK_WIDGET (self->overview_counts));
    for (guint i = 0; i < NAUTILUS_USAGE_SEGMENTS; i++)
    {
        GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
        self->overview_rows[i] = row;
        GtkWidget *dot = gtk_label_new (NULL);
        g_autofree char *markup = g_strdup_printf ("<span foreground=\"%s\">●</span>", usage_colors[i]);
        gtk_label_set_markup (GTK_LABEL (dot), markup);
        gtk_box_append (GTK_BOX (row), dot);
        self->overview_names[i] = GTK_LABEL (gtk_label_new (NULL));
        gtk_label_set_xalign (self->overview_names[i], 0);
        gtk_label_set_ellipsize (self->overview_names[i], PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_set_hexpand (GTK_WIDGET (self->overview_names[i]), TRUE);
        gtk_box_append (GTK_BOX (row), GTK_WIDGET (self->overview_names[i]));
        self->overview_values[i] = GTK_LABEL (gtk_label_new (NULL));
        gtk_widget_add_css_class (GTK_WIDGET (self->overview_values[i]), "caption");
        gtk_box_append (GTK_BOX (row), GTK_WIDGET (self->overview_values[i]));
        gtk_box_append (GTK_BOX (overview), row);
    }
    GtkWidget *note = gtk_label_new ("Includes nested and hidden items. File sizes are logical sizes; links are counted but not followed. The six largest subfolders are shown separately.");
    gtk_label_set_wrap (GTK_LABEL (note), TRUE);
    gtk_widget_add_css_class (note, "dim-label");
    gtk_widget_add_css_class (note, "caption");
    gtk_box_append (GTK_BOX (overview), note);
    self->overview_refresh = gtk_button_new_with_label ("Rescan folder");
    g_signal_connect (self->overview_refresh, "clicked", G_CALLBACK (overview_refresh_clicked), self);
    gtk_box_append (GTK_BOX (overview), self->overview_refresh);
    GtkWidget *overview_scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (overview_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (overview_scroll), overview);
    gtk_stack_add_named (self->stack, overview_scroll, "overview");
    self->picture = GTK_PICTURE (gtk_picture_new ());
    gtk_picture_set_can_shrink (self->picture, TRUE);
    gtk_picture_set_content_fit (self->picture, GTK_CONTENT_FIT_CONTAIN);
    gtk_stack_add_named (self->stack, GTK_WIDGET (self->picture), "image");
    self->video = GTK_VIDEO (gtk_video_new ());
    gtk_video_set_autoplay (self->video, FALSE);
    gtk_video_set_loop (self->video, FALSE);
    gtk_widget_set_size_request (GTK_WIDGET (self->video), 240, 160);
    gtk_widget_set_hexpand (GTK_WIDGET (self->video), TRUE);
    gtk_widget_set_vexpand (GTK_WIDGET (self->video), TRUE);
    gtk_stack_add_named (self->stack, GTK_WIDGET (self->video), "video");
    self->text = GTK_TEXT_VIEW (gtk_text_view_new ());
    gtk_text_view_set_editable (self->text, FALSE);
    gtk_text_view_set_cursor_visible (self->text, FALSE);
    gtk_text_view_set_wrap_mode (self->text, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin (self->text, 10);
    gtk_text_view_set_right_margin (self->text, 10);
    gtk_text_view_set_top_margin (self->text, 10);
    gtk_text_view_set_bottom_margin (self->text, 10);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer (self->text);
    gtk_text_buffer_create_tag (buffer, "heading1", "weight", PANGO_WEIGHT_BOLD, "scale", 1.5, NULL);
    gtk_text_buffer_create_tag (buffer, "heading2", "weight", PANGO_WEIGHT_BOLD, "scale", 1.2, NULL);
    gtk_text_buffer_create_tag (buffer, "strong", "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag (buffer, "emphasis", "style", PANGO_STYLE_ITALIC, NULL);
    gtk_text_buffer_create_tag (buffer, "code", "family", "monospace", NULL);
    gtk_text_buffer_create_tag (buffer, "link", "underline", PANGO_UNDERLINE_SINGLE, NULL);
    gtk_text_buffer_create_tag (buffer, "quote", "left-margin", 14, "style", PANGO_STYLE_ITALIC, NULL);
    GtkWidget *scroll = gtk_scrolled_window_new ();
    self->text_scroll = scroll;
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), GTK_WIDGET (self->text));
    gtk_stack_add_named (self->stack, scroll, "text");
    self->folder_items = g_list_store_new (G_TYPE_FILE_INFO);
    GtkListItemFactory *folder_factory = gtk_signal_list_item_factory_new ();
    g_signal_connect (folder_factory, "setup", G_CALLBACK (folder_row_setup), NULL);
    g_signal_connect (folder_factory, "bind", G_CALLBACK (folder_row_bind), NULL);
    self->folder_list = GTK_LIST_VIEW (gtk_list_view_new (
        GTK_SELECTION_MODEL (gtk_no_selection_new (G_LIST_MODEL (g_object_ref (self->folder_items)))), folder_factory));
    gtk_widget_add_css_class (GTK_WIDGET (self->folder_list), "boxed-list");
    self->folder_scroll = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
    gtk_scrolled_window_set_policy (self->folder_scroll, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (self->folder_scroll, GTK_WIDGET (self->folder_list));
    gtk_widget_set_vexpand (GTK_WIDGET (self->folder_scroll), TRUE);
    GtkAdjustment *folder_adjustment = gtk_scrolled_window_get_vadjustment (self->folder_scroll);
    g_signal_connect (folder_adjustment, "value-changed", G_CALLBACK (folder_scroll_changed), self);
    g_signal_connect (folder_adjustment, "changed", G_CALLBACK (folder_scroll_changed), self);
    GtkEventController *folder_motion = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    gtk_event_controller_set_propagation_phase (folder_motion, GTK_PHASE_CAPTURE);
    g_signal_connect (folder_motion, "scroll", G_CALLBACK (folder_scroll_input), self);
    gtk_widget_add_controller (GTK_WIDGET (self->folder_scroll), folder_motion);
    GtkWidget *folder_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append (GTK_BOX (folder_box), GTK_WIDGET (self->folder_scroll));
    self->folder_status = GTK_LABEL (gtk_label_new (NULL));
    gtk_label_set_wrap (self->folder_status, TRUE);
    gtk_widget_add_css_class (GTK_WIDGET (self->folder_status), "dim-label");
    gtk_box_append (GTK_BOX (folder_box), GTK_WIDGET (self->folder_status));
    self->folder_more = gtk_button_new_with_label ("Load more");
    g_signal_connect (self->folder_more, "clicked", G_CALLBACK (folder_more_clicked), self);
    gtk_box_append (GTK_BOX (folder_box), self->folder_more);
    gtk_stack_add_named (self->stack, folder_box, "folder");
    self->sheet = WEBKIT_WEB_VIEW (webkit_web_view_new ());
    WebKitSettings *web_settings = webkit_web_view_get_settings (self->sheet);
    webkit_settings_set_enable_javascript (web_settings, FALSE);
    webkit_settings_set_auto_load_images (web_settings, FALSE);
    webkit_settings_set_enable_developer_extras (web_settings, FALSE);
    gtk_widget_set_size_request (GTK_WIDGET (self->sheet), 240, 160);
    gtk_stack_add_named (self->stack, GTK_WIDGET (self->sheet), "sheet");
    gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->stack));
    self->page_controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    self->previous_page = gtk_button_new_from_icon_name ("go-previous-symbolic");
    gtk_widget_set_tooltip_text (self->previous_page, "Previous PDF page");
    g_signal_connect (self->previous_page, "clicked", G_CALLBACK (previous_page_clicked), self);
    self->page_label = GTK_LABEL (gtk_label_new (""));
    gtk_widget_set_hexpand (GTK_WIDGET (self->page_label), TRUE);
    self->next_page = gtk_button_new_from_icon_name ("go-next-symbolic");
    gtk_widget_set_tooltip_text (self->next_page, "Next PDF page");
    g_signal_connect (self->next_page, "clicked", G_CALLBACK (next_page_clicked), self);
    gtk_box_append (GTK_BOX (self->page_controls), self->previous_page);
    gtk_box_append (GTK_BOX (self->page_controls), GTK_WIDGET (self->page_label));
    gtk_box_append (GTK_BOX (self->page_controls), self->next_page);
    gtk_widget_set_visible (self->page_controls, FALSE);
    gtk_box_append (GTK_BOX (self), self->page_controls);
    self->open = gtk_button_new_with_label ("Open in application");
    gtk_widget_set_sensitive (self->open, FALSE);
    g_signal_connect (self->open, "clicked", G_CALLBACK (open_file), self);
    gtk_box_append (GTK_BOX (self), self->open);
    g_signal_connect_object (gtk_filechooser_preferences,
                             "changed::" NAUTILUS_PREFERENCES_SHOW_HIDDEN_FILES,
                             G_CALLBACK (folder_visibility_changed), self, 0);
}

GtkWidget *
nautilus_preview_pane_new (void)
{
    return g_object_new (NAUTILUS_TYPE_PREVIEW_PANE, NULL);
}
