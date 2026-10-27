# Architecture

`src/nautilus-window-slot.c` places an existing Nautilus slot and the preview widget in a horizontal `GtkPaned`. Selection notifications update the preview. The slot owns the pane and cancels pending work during disposal. Visibility is stored in the local build's Nautilus GSettings schema.

`src/nautilus-preview-pane.c` implements the GTK widget, the 120 ms selection debounce, asynchronous work, serial numbers that reject stale results, page navigation, and renderer dispatch.

| File type | Preview path |
| --- | --- |
| Folders | Cancellable GIO enumeration off the main thread → sorted, read-only GTK list |
| Text/Markdown | Bounded file reads, GtkTextView, local CommonMark rendering |
| Images | Glycin sandboxed decode, GtkPicture |
| PDF | Poppler page rasterization, Glycin texture loading |
| DOCX/ODT/DOC/RTF | Headless LibreOffice → PDF → page rasterization |
| XLSX/XLS/ODS/CSV/TSV | Local workbook parsing → escaped HTML → WebKitGTK grid |

The helper scripts install under `${prefix}/share/nautilus/smok-glass/`. The native binary resolves the helper relative to its configured Nautilus data directory; there are no developer-home paths in runtime code.

Office subprocesses use a private process group, allowing cancellation to terminate both the helper and its renderer. Work directories are private and removed without following symlinks. LibreOffice runs through Bubblewrap with networking disabled, read-only source mounts, a private runtime/cache, and a profile that disables macros.

The spreadsheet view has JavaScript disabled and a restrictive Content Security Policy. Sheet switching uses local radio controls and CSS. Cell contents are escaped before rendering, and formulas use cached values rather than executing spreadsheet calculations.

`scripts/launchers.py` manages the optional per-user launcher overrides and records backups under `$XDG_STATE_HOME/smok-glass-nautilus`. It does not replace `/usr/bin/nautilus`.

The source tree includes original upstream components and tests. `src/nautilus-preview-check.c` provides extra native integration checks. `tests/test_document_previews.py` tests the helper layer without requiring a display.

Folder enumeration reads only immediate child metadata without following child symlinks. It caps visible results at 500 and scanned entries at 10,000, then sorts the bounded result with folders first and natural filename collation. Hidden/backup entries follow the GTK file chooser preference, including changes while a folder is selected. No recursive size calculation or file-content reads are performed.
