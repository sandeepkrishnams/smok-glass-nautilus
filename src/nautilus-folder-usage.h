/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>

#define NAUTILUS_USAGE_DEFAULT_ITEMS 5000000
#define NAUTILUS_USAGE_DEFAULT_SECONDS 120
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
    gboolean running, partial, unreadable, limited, cancelled, depth_limited;
} NautilusUsageResult;

typedef struct _NautilusFolderUsage NautilusFolderUsage;
NautilusFolderUsage *nautilus_folder_usage_new (GFile *location);
/* Set before starting the worker; zero disables that limit. Depth remains bounded. */
void nautilus_folder_usage_set_limits (NautilusFolderUsage *usage, guint64 items, guint seconds);
void nautilus_folder_usage_free (NautilusFolderUsage *usage);
/* Run on a worker thread. Snapshot is safe to call from the UI thread. */
void nautilus_folder_usage_scan (NautilusFolderUsage *usage, GCancellable *cancellable);
void nautilus_folder_usage_snapshot (NautilusFolderUsage *usage, NautilusUsageResult *result);
