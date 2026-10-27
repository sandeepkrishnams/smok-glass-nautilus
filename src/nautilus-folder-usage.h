/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>

#define NAUTILUS_USAGE_TOP_FOLDERS 6
#define NAUTILUS_USAGE_SEGMENTS (NAUTILUS_USAGE_TOP_FOLDERS + 2)

typedef struct
{
    char name[512];
    guint64 bytes;
} NautilusUsageSegment;

typedef struct
{
    guint64 bytes, files, folders, skipped;
    guint n_segments;
    NautilusUsageSegment segments[NAUTILUS_USAGE_SEGMENTS];
    gboolean running, partial, unreadable, limited, cancelled;
} NautilusUsageResult;

typedef struct _NautilusFolderUsage NautilusFolderUsage;
NautilusFolderUsage *nautilus_folder_usage_new (GFile *location);
void nautilus_folder_usage_free (NautilusFolderUsage *usage);
/* Run on a worker thread. Snapshot is safe to call from the UI thread. */
void nautilus_folder_usage_scan (NautilusFolderUsage *usage, GCancellable *cancellable);
void nautilus_folder_usage_snapshot (NautilusFolderUsage *usage, NautilusUsageResult *result);
