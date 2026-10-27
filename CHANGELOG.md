# Changelog

## Unreleased

- Raised folder-usage defaults to 5 million items / 120 seconds, with persistent item/time controls under Preferences → Folder Usage Preview and a per-scan bypass button. Partial sizes now show ≥ and item counts say counted so far; Stop/cancellation and bounded traversal depth remain available.

- An unselected folder now shows a coloured breakdown of its subfolder sizes, loose files, total size, and recursive item count. Metadata scans run in the background with bounded aggregates, cancellation, progress, Stop/Rescan controls, short-lived caching, network opt-in, and labelled partial totals.

- Local ZIP/ZIP64 archive contents now preview in 32-entry pages without extraction, with stored paths, uncompressed sizes, encrypted-entry labels, and bounded central-directory metadata.

- Folder previews now load the first 32 entries and fetch subsequent batches as you scroll, with a virtualized list, Load more control, bounded hidden-item scans, and no 500-item total cap.
- Folder counts describe loaded items; folder-first ordering applies within each batch so loading does not wait for a complete directory scan.

- Local video previews with native playback controls, duration, seeking, paused loading, and cleanup on selection changes or hiding.
- Delayed hover previews in grid/list views without changing selection, restoring the selected preview when the pointer leaves.

- Selecting a folder previews its files and subfolders, with icons, sizes, folder-first batch sorting, and an item summary.
- Folder listings also work with GIO-backed locations, including mounted SMB shares, and handle optional network metadata.
- Folder previews respect hidden-item visibility, show empty/error states, and cap large listings while keeping selection cancellable.

## 0.1.0 — 2026-10-27

- Initial source release based on Nautilus 50.2.2.
- Embedded resizable preview pane with Ctrl+Shift+P.
- Text, Markdown, images, formatted document pages, and spreadsheet grids.
- Read-only office rendering, page navigation, sheet tabs, preview limits, and cancellation.
- Per-user installation and launcher restore tools.
- Sample documents, native preview checks, screenshots, and GitHub CI configuration.
