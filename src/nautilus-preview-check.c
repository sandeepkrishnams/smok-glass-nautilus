/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Integration checks exercise the actual widget, loader and Markdown renderer. */
#include "nautilus-preview-pane.c"
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
        while (g_main_context_iteration (NULL, FALSE));
        g_usleep (1000);
    } while (g_get_monotonic_time () < until);
}

static NautilusFile *
ready_file (const char *path)
{
    g_autoptr (GFile) location = g_file_new_for_path (path);
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
