/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nautilus-folder-usage.h"
#include <string.h>
#include <stdlib.h>

#define USAGE_MAX_DEPTH 64
#define USAGE_MAX_ITEMS 200000
#define USAGE_MAX_TIME (30 * G_USEC_PER_SEC)

struct _NautilusFolderUsage
{
    GFile *location;
    GMutex mutex;
    NautilusUsageResult published, totals;
    NautilusUsageSegment top[NAUTILUS_USAGE_TOP_FOLDERS], current;
    guint top_count, other_count, loose_count;
    guint64 other_bytes, loose_bytes;
    gint64 started, last_publish;
    gboolean current_active, halted;
};

NautilusFolderUsage *
nautilus_folder_usage_new (GFile *location)
{
    NautilusFolderUsage *usage = g_new0 (NautilusFolderUsage, 1);
    usage->location = g_object_ref (location);
    g_mutex_init (&usage->mutex);
    usage->published.running = TRUE;
    return usage;
}

void
nautilus_folder_usage_free (NautilusFolderUsage *usage)
{
    g_object_unref (usage->location);
    g_mutex_clear (&usage->mutex);
    g_free (usage);
}

void
nautilus_folder_usage_snapshot (NautilusFolderUsage *usage, NautilusUsageResult *result)
{
    g_mutex_lock (&usage->mutex);
    *result = usage->published;
    g_mutex_unlock (&usage->mutex);
}

static int
segment_compare (gconstpointer a, gconstpointer b)
{
    const NautilusUsageSegment *left = a, *right = b;
    if (left->bytes != right->bytes) return left->bytes > right->bytes ? -1 : 1;
    return strcmp (left->name, right->name);
}

/* Keep only the largest six folders; memory does not grow with breadth. */
static void
add_folder (NautilusUsageSegment *top, guint *count, guint64 *other_bytes,
            guint *other_count, const NautilusUsageSegment *folder)
{
    NautilusUsageSegment candidates[NAUTILUS_USAGE_TOP_FOLDERS + 1];
    memcpy (candidates, top, *count * sizeof *top);
    guint n = *count + 1;
    candidates[*count] = *folder;
    qsort (candidates, n, sizeof *top, segment_compare);
    if (n > NAUTILUS_USAGE_TOP_FOLDERS)
    {
        *other_bytes += candidates[NAUTILUS_USAGE_TOP_FOLDERS].bytes;
        (*other_count)++;
    }
    *count = MIN (n, NAUTILUS_USAGE_TOP_FOLDERS);
    memcpy (top, candidates, *count * sizeof *top);
}

static void
publish (NautilusFolderUsage *usage, gboolean force)
{
    gint64 now = g_get_monotonic_time ();
    if (!force && now - usage->last_publish < 100 * 1000) return;
    NautilusUsageResult result = usage->totals;
    result.n_segments = usage->top_count;
    memcpy (result.segments, usage->top, usage->top_count * sizeof *usage->top);
    guint other_count = usage->other_count;
    guint64 other_bytes = usage->other_bytes;
    if (usage->current_active)
        add_folder (result.segments, &result.n_segments, &other_bytes, &other_count, &usage->current);
    if (other_count > 0)
    {
        NautilusUsageSegment *segment = &result.segments[result.n_segments++];
        g_strlcpy (segment->name, "Other folders", sizeof segment->name);
        segment->bytes = other_bytes;
    }
    if (usage->loose_count > 0)
    {
        NautilusUsageSegment *segment = &result.segments[result.n_segments++];
        g_strlcpy (segment->name, "Loose files", sizeof segment->name);
        segment->bytes = usage->loose_bytes;
    }
    g_mutex_lock (&usage->mutex);
    usage->published = result;
    g_mutex_unlock (&usage->mutex);
    usage->last_publish = now;
}

static gboolean
can_continue (NautilusFolderUsage *usage, GCancellable *cancellable)
{
    if (g_cancellable_is_cancelled (cancellable))
    {
        usage->totals.cancelled = usage->totals.partial = usage->halted = TRUE;
    }
    else if (usage->totals.files + usage->totals.folders >= USAGE_MAX_ITEMS ||
             g_get_monotonic_time () - usage->started >= USAGE_MAX_TIME)
    {
        usage->totals.limited = usage->totals.partial = usage->halted = TRUE;
    }
    return !usage->halted;
}

static void
record_error (NautilusFolderUsage *usage, GCancellable *cancellable)
{
    if (!g_cancellable_is_cancelled (cancellable))
    {
        usage->totals.partial = TRUE;
        usage->totals.skipped++;
    }
}

/* One sequential depth-first walk: at most 64 open enumerators, one entry per
 * level, and fixed-size aggregate data. No payload reads or symlink following. */
static void
scan_directory (NautilusFolderUsage *usage, GFile *directory, guint depth, GCancellable *cancellable)
{
    if (!can_continue (usage, cancellable)) return;
    if (depth >= USAGE_MAX_DEPTH)
    {
        usage->totals.limited = usage->totals.partial = TRUE;
        usage->totals.skipped++;
        return;
    }
    g_autoptr (GError) error = NULL;
    g_autoptr (GFileEnumerator) enumerator = g_file_enumerate_children (directory,
        G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","
        G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK ","
        G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, &error);
    if (enumerator == NULL)
    {
        record_error (usage, cancellable);
        if (depth == 0 && !g_cancellable_is_cancelled (cancellable)) usage->totals.unreadable = TRUE;
        return;
    }
    while (can_continue (usage, cancellable))
    {
        g_autoptr (GFileInfo) info = g_file_enumerator_next_file (enumerator, cancellable, &error);
        if (info == NULL)
        {
            if (error != NULL) record_error (usage, cancellable);
            break;
        }
        const char *name = g_file_info_get_name (info);
        if (name == NULL || *name == '\0' || strchr (name, '/') != NULL ||
            g_str_equal (name, ".") || g_str_equal (name, ".."))
        {
            record_error (usage, cancellable);
            continue;
        }
        gboolean directory_entry = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY &&
                                   !g_file_info_get_is_symlink (info);
        if (directory_entry)
        {
            usage->totals.folders++;
            if (depth == 0)
            {
                memset (&usage->current, 0, sizeof usage->current);
                const char *display = g_file_info_get_display_name (info);
                g_autofree char *valid = g_utf8_make_valid (display != NULL ? display : name, -1);
                /* Bound names and flatten controls so filenames cannot spoof legend rows. */
                for (char *c = valid; *c; c++) if ((guchar) *c < 32 || *c == 127) *c = ' ';
                g_autofree char *short_name = g_utf8_substring (valid, 0, MIN (80, g_utf8_strlen (valid, -1)));
                g_snprintf (usage->current.name, sizeof usage->current.name, "%s/", short_name);
                usage->current_active = TRUE;
            }
            g_autoptr (GFile) child = g_file_get_child (directory, name);
            scan_directory (usage, child, depth + 1, cancellable);
            if (depth == 0)
            {
                add_folder (usage->top, &usage->top_count, &usage->other_bytes, &usage->other_count, &usage->current);
                usage->current_active = FALSE;
            }
        }
        else
        {
            usage->totals.files++;
            /* Logical regular-file sizes; links and special files contribute no bytes. */
            if (g_file_info_get_file_type (info) == G_FILE_TYPE_UNKNOWN ||
                (g_file_info_get_file_type (info) == G_FILE_TYPE_REGULAR &&
                 !g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_SIZE)))
                record_error (usage, cancellable);
            goffset size = g_file_info_get_size (info);
            guint64 bytes = g_file_info_get_file_type (info) == G_FILE_TYPE_REGULAR &&
                !g_file_info_get_is_symlink (info) && size > 0 ? (guint64) size : 0;
            if (G_MAXUINT64 - usage->totals.bytes < bytes)
            {
                usage->totals.limited = usage->totals.partial = usage->halted = TRUE;
                break;
            }
            usage->totals.bytes += bytes;
            if (depth == 0) { usage->loose_bytes += bytes; usage->loose_count++; }
            else usage->current.bytes += bytes;
        }
        publish (usage, FALSE);
    }
    if (error != NULL && depth == 0 && usage->totals.files + usage->totals.folders == 0)
        usage->totals.unreadable = TRUE;
    g_file_enumerator_close (enumerator, NULL, NULL);
}

void
nautilus_folder_usage_scan (NautilusFolderUsage *usage, GCancellable *cancellable)
{
    usage->started = g_get_monotonic_time ();
    usage->totals.running = TRUE;
    scan_directory (usage, usage->location, 0, cancellable);
    if (g_cancellable_is_cancelled (cancellable)) usage->totals.cancelled = usage->totals.partial = TRUE;
    usage->totals.running = FALSE;
    publish (usage, TRUE);
}
