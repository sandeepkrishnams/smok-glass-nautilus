/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gtk/gtk.h>
#include "nautilus-file.h"
G_BEGIN_DECLS
#define NAUTILUS_TYPE_PREVIEW_PANE (nautilus_preview_pane_get_type ())
G_DECLARE_FINAL_TYPE (NautilusPreviewPane, nautilus_preview_pane, NAUTILUS, PREVIEW_PANE, GtkBox)
GtkWidget *nautilus_preview_pane_new (void);
void nautilus_preview_pane_set_files (NautilusPreviewPane *self, GList *files);
void nautilus_preview_pane_set_context (NautilusPreviewPane *self, GFile *location, GListModel *model, gboolean loading);
void nautilus_preview_pane_cancel (NautilusPreviewPane *self);
G_END_DECLS
