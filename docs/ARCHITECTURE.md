# Architecture

`src/nautilus-window-slot.c` places an existing Nautilus slot and the preview widget in a horizontal `GtkPaned`. Selection notifications update the preview. The slot owns the pane and cancels pending work during disposal. Visibility is stored in the local build's Nautilus GSettings schema.

`src/nautilus-preview-pane.c` implements the GTK widget, the 120 ms selection debounce, asynchronous work, serial numbers that reject stale results, page navigation, and renderer dispatch.

| File type | Preview path |
| --- | --- |
| Folders | Cursor-based GIO enumeration in bounded worker batches → GListStore → virtualized GtkListView |
| Text/Markdown | Bounded file reads, GtkTextView, local CommonMark rendering |
| Local videos | Debounced GtkMediaFile → GtkVideo with GStreamer playback controls |
| Images | Glycin sandboxed decode, GtkPicture |
| PDF | Poppler page rasterization, Glycin texture loading |
| DOCX/ODT/DOC/RTF | Headless LibreOffice → PDF → page rasterization |
| XLSX/XLS/ODS/CSV/TSV | Local workbook parsing → escaped HTML → WebKitGTK grid |

The helper scripts install under `${prefix}/share/nautilus/smok-glass/`. The native binary resolves the helper relative to its configured Nautilus data directory; there are no developer-home paths in runtime code.

Office subprocesses use a private process group, allowing cancellation to terminate both the helper and its renderer. Work directories are private and removed without following symlinks. LibreOffice runs through Bubblewrap with networking disabled, read-only source mounts, a private runtime/cache, and a profile that disables macros.

The spreadsheet view has JavaScript disabled and a restrictive Content Security Policy. Sheet switching uses local radio controls and CSS. Cell contents are escaped before rendering, and formulas use cached values rather than executing spreadsheet calculations.

`scripts/launchers.py` manages the optional per-user launcher overrides and records backups under `$XDG_STATE_HOME/smok-glass-nautilus`. It does not replace `/usr/bin/nautilus`.

The source tree includes original upstream components and tests. `src/nautilus-preview-check.c` provides extra native integration checks. `tests/test_document_previews.py` tests the helper layer without requiring a display.

Folder enumeration supports local and GIO-backed locations, including mounted network folders. The first worker requests at most 32 visible children and scans at most 256 entries; it does not enumerate the whole directory for a total or a global sort. Hidden-only batches expose an explicit continuation so a heavily filtered directory does not cause an unbounded initial scan. The GIO backend may internally prefetch metadata.

The enumerator is owned by either the pane or one worker at a time. Scrolling within 120 pixels of the loaded bottom (or pressing Load more) transfers that cursor to the next worker. The existing rows remain visible while it loads. Success appends one batch to GListStore, with folder-first natural ordering within that batch. Already loaded rows do not move. GtkListView reuses row widgets for visible/nearby items; metadata is retained only for pages already requested. There is no 500-item cap, and the summary is explicitly a loaded count until end-of-directory.

Hidden/backup entries follow the GTK file chooser preference; changing it cancels the worker and starts a new cursor at the top. Selection changes, pane hiding, and disposal cancel pending work, and serial numbers reject stale pages. Cursor close is asynchronous so network cleanup does not block GTK. Later read errors preserve loaded rows and offer Retry loading. Child symlinks are listed without following them; there is no recursion or file-content reading. See the [GTK list view documentation](https://docs.gtk.org/gtk4/class.ListView.html) for the factory/recycling model.

Video streams load paused after the selection debounce. Prepared/error notifications update only the current stream; cancellation disconnects notifications, pauses, detaches, clears, and releases it. Videos are decoded by the installed GTK media backend, not Glycin or the office sandbox. They are local-only and do not use the image/document size cap.

A capture-phase pointer motion controller on the slot’s content stack resolves grid/list cells without changing selection. A 350 ms dwell precedes the existing 120 ms loader debounce. Crossing items replaces pending hover work; leaving restores the selection. Selection, navigation, hiding, and disposal clear hover state. Pointer motion with pressed mouse buttons does not initiate previews.
