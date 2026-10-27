/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"
#include "nautilus-preview-pane.h"
#include "nautilus-global-preferences.h"
#include <adwaita.h>
#include <cmark.h>
#include <glycin.h>
#include <glycin-gtk4.h>
#include <string.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <unistd.h>
#include <webkit/webkit.h>

#define TEXT_LIMIT (512 * 1024)
#define FOLDER_LIMIT 500
#define FOLDER_SCAN_LIMIT 10000

struct _NautilusPreviewPane
{
    GtkBox parent_instance;
    GtkLabel *name;
    GtkLabel *metadata;
    GtkLabel *message;
    GtkStack *stack;
    GtkPicture *picture;
    GtkTextView *text;
    GtkToggleButton *source;
    GtkWidget *open;
    GtkWidget *text_scroll;
    GtkWidget *page_controls;
    GtkWidget *previous_page;
    GtkWidget *next_page;
    GtkLabel *page_label;
    WebKitWebView *sheet;
    GtkListBox *folder_list;
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
} PreviewLoad;

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
preview_load_free (gpointer data)
{
    PreviewLoad *load = data;
    g_clear_object (&load->file);
    g_clear_object (&load->frame);
    g_free (load->text);
    g_free (load->document_kind);
    g_clear_pointer (&load->entries, g_ptr_array_unref);
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
    g_autoptr (GFileEnumerator) enumerator = g_file_enumerate_children (
        load->file, "standard::name,standard::display-name,standard::type,standard::size,"
                    "standard::icon,standard::is-hidden,standard::is-backup,standard::is-symlink",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, error);
    if (enumerator == NULL) return FALSE;
    load->entries = g_ptr_array_new_with_free_func (g_object_unref);
    for (guint scanned = 0; scanned <= FOLDER_SCAN_LIMIT; scanned++)
    {
        g_autoptr (GFileInfo) info = g_file_enumerator_next_file (enumerator, cancel, error);
        if (info == NULL) return *error == NULL;
        if (scanned == FOLDER_SCAN_LIMIT)
        {
            load->truncated = TRUE;
            break;
        }
        if (!load->show_hidden && (g_file_info_get_is_hidden (info) || g_file_info_get_is_backup (info))) continue;
        if (load->entries->len == FOLDER_LIMIT)
        {
            load->truncated = TRUE;
            break;
        }
        g_ptr_array_add (load->entries, g_steal_pointer (&info));
    }
    return TRUE;
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
        if (!load_folder (load, cancel, &error))
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

static void
preview_loaded (GObject *object, GAsyncResult *result, gpointer data)
{
    NautilusPreviewPane *self = NAUTILUS_PREVIEW_PANE (object);
    GTask *task = G_TASK (result);
    PreviewLoad *load = g_task_get_task_data (task);
    g_autoptr (GError) error = NULL;
    gboolean success = g_task_propagate_boolean (task, &error);
    if (load->serial != self->serial) return;
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
        guint folders = 0;
        for (guint i = 0; i < load->entries->len; i++)
        {
            GFileInfo *info = g_ptr_array_index (load->entries, i);
            gboolean directory = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;
            folders += directory;
            GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
            gtk_widget_set_margin_top (row, 8);
            gtk_widget_set_margin_bottom (row, 8);
            gtk_widget_set_margin_start (row, 8);
            gtk_widget_set_margin_end (row, 8);
            GIcon *icon = g_file_info_get_icon (info);
            GtkWidget *image = g_file_info_get_is_symlink (info) ? gtk_image_new_from_icon_name ("insert-link-symbolic") :
                               icon != NULL ? gtk_image_new_from_gicon (icon)
                                           : gtk_image_new_from_icon_name (directory ? "folder-symbolic" : "text-x-generic-symbolic");
            gtk_image_set_pixel_size (GTK_IMAGE (image), 24);
            gtk_box_append (GTK_BOX (row), image);
            GtkWidget *label = gtk_label_new (g_file_info_get_display_name (info));
            gtk_label_set_xalign (GTK_LABEL (label), 0);
            gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_MIDDLE);
            gtk_widget_set_hexpand (label, TRUE);
            gtk_widget_set_tooltip_text (label, g_file_info_get_display_name (info));
            gtk_box_append (GTK_BOX (row), label);
            g_autofree char *detail = directory ? g_strdup ("Folder") :
                g_file_info_get_is_symlink (info) ? g_strdup ("Link") :
                g_file_info_get_file_type (info) == G_FILE_TYPE_REGULAR ? g_format_size (g_file_info_get_size (info)) : g_strdup ("File");
            GtkWidget *size = gtk_label_new (detail);
            gtk_widget_add_css_class (size, "dim-label");
            gtk_widget_add_css_class (size, "caption");
            gtk_box_append (GTK_BOX (row), size);
            gtk_list_box_append (self->folder_list, row);
        }
        g_autofree char *summary = g_strdup_printf ("%u %s · %u %s%s", folders, folders == 1 ? "folder" : "folders",
            load->entries->len - folders, load->entries->len - folders == 1 ? "file" : "files", load->show_hidden ? " · including hidden items" : " · hidden items excluded");
        g_autofree char *metadata = load->truncated ? g_strdup_printf ("Showing %u items · listing limited\n%s", load->entries->len, summary) : g_strdup (summary);
        gtk_label_set_text (self->metadata, metadata);
        if (load->entries->len == 0)
        {
            gtk_label_set_text (self->message, load->truncated ? "No visible items within the preview limit. Open the folder to browse everything." :
                                load->show_hidden ? "This folder is empty." : "No visible items in this folder.");
            gtk_stack_set_visible_child_name (self->stack, "message");
        }
        else gtk_stack_set_visible_child_name (self->stack, "folder");
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

void
nautilus_preview_pane_cancel (NautilusPreviewPane *self)
{
    self->serial++;
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
    load->show_hidden = g_settings_get_boolean (gtk_filechooser_preferences, NAUTILUS_PREFERENCES_SHOW_HIDDEN_FILES);
    g_autoptr (GTask) task = g_task_new (self, self->cancellable, preview_loaded, NULL);
    g_task_set_task_data (task, load, preview_load_free);
    self->load_source = g_timeout_add_full (G_PRIORITY_DEFAULT, 120, start_preview,
                                          g_object_ref (task), release_pending_preview);
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
    if (!self->folder || self->file == NULL) return;
    nautilus_preview_pane_cancel (self);
    gtk_list_box_remove_all (self->folder_list);
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
    gtk_list_box_remove_all (self->folder_list);
    gtk_widget_set_visible (GTK_WIDGET (self->source), FALSE);
    gtk_widget_set_visible (self->page_controls, FALSE);
    self->document_kind = NULL;
    self->folder = FALSE;
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
    if (!g_file_is_native (self->file))
    {
        gtk_label_set_text (self->message, "Open this remote file to view its contents.");
        return;
    }
    if (nautilus_file_is_directory (file))
    {
        self->folder = TRUE;
        gtk_label_set_text (self->metadata, "Folder · loading contents…");
        gtk_button_set_label (GTK_BUTTON (self->open), "Open folder");
        queue_preview (self, FALSE);
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
    self->picture = GTK_PICTURE (gtk_picture_new ());
    gtk_picture_set_can_shrink (self->picture, TRUE);
    gtk_picture_set_content_fit (self->picture, GTK_CONTENT_FIT_CONTAIN);
    gtk_stack_add_named (self->stack, GTK_WIDGET (self->picture), "image");
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
    self->folder_list = GTK_LIST_BOX (gtk_list_box_new ());
    gtk_list_box_set_selection_mode (self->folder_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class (GTK_WIDGET (self->folder_list), "boxed-list");
    GtkWidget *folder_scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (folder_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (folder_scroll), GTK_WIDGET (self->folder_list));
    gtk_stack_add_named (self->stack, folder_scroll, "folder");
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
