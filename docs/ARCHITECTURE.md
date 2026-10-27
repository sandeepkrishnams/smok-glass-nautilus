# Architecture

`src/nautilus-window-slot.c` places an existing Nautilus slot and the preview widget in a horizontal `GtkPaned`. Selection notifications update the preview. The slot owns the pane and cancels pending work during disposal. Visibility is stored in the local build's Nautilus GSettings schema.

`src/nautilus-preview-pane.c` implements the GTK widget, the 120 ms selection debounce, asynchronous work, serial numbers that reject stale results, page navigation, and renderer dispatch.

| File type | Preview path |
| --- | --- |
| Folders | Cursor-based GIO enumeration in bounded worker batches → GListStore → virtualized GtkListView |
| Text/Markdown | Bounded file reads, GtkTextView, local CommonMark rendering |
| Local ZIP archives | Bounded ZIP/ZIP64 index check → libarchive seekable reader → paged, virtualized entry list |
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

ZIP previews reuse the folder list/model and paging controls with a separate libarchive cursor. The worker opens only regular local files, validates the end-of-directory record (including ZIP64), and rejects central indices above 16 MiB or more than 100,000 declared entries before libarchive indexing. The reader also enforces the entry limit while paging. Libarchive’s seekable ZIP reader can index the central metadata up front; row creation and entry presentation remain lazy. No archive extraction or regular-member data reads are requested. Link metadata may be read by libarchive. Stored entry paths are plain display strings, never filesystem destinations; controls and long display names are bounded. Encrypted entry data can still have readable names/sizes; inaccessible metadata falls back with an error, without a password dialog. The cursor is transferred between exactly one worker and the pane; cancellation/stale-result rules match folders, and cleanup closes the reader/file descriptor off the GTK thread. See [libarchive](https://www.libarchive.org/) for the underlying parser.

The idle folder overview receives the current location/loading state from the slot. `src/nautilus-folder-usage.c` performs a sequential depth-first GIO metadata scan on a GTask worker. It sums logical sizes of regular files, counts all descendants (including hidden items), does not request payloads, and enumerates child metadata with NOFOLLOW_SYMLINKS. Child links and special files count as items but add zero bytes. Hard links count per path. This is a live metadata snapshot, not an atomic filesystem snapshot; concurrent mutations can affect totals.

The worker retains six largest immediate subfolder aggregates, a remainder, and loose files. A mutex protects one fixed-size progress snapshot; the UI reads it at most every 150 ms and reuses eight legend rows plus a native Cairo donut. Memory scales with traversal depth rather than number of descendants, although GIO backends can internally prefetch. Depth is limited to 64 open directory enumerators, item count to 200,000, and elapsed scan time to 30 seconds checked between GIO operations. Backend operations and cursor cleanup can exceed that time on an unresponsive server. Unreadable paths and limits mark results partial; cancellation interrupts supported GIO operations. No widget is accessed from the worker.

Local scans start after a 450 ms debounce and wait for the main folder view to finish loading. Network scans require Calculate folder usage. Selection, navigation, hiding, or disposal cancel work and remove progress/debounce sources. Task-identity checks reject stale completions. Stop preserves partial totals and Rescan starts a fresh traversal. Completed results are cached for 30 seconds per context on restoration, with no periodic scan. Existing view-model changes invalidate this cache without restarting a running traversal; a completed view offers Rescan. Model handlers are disconnected on context changes/disposal. Virtual location views keep the selection prompt.
