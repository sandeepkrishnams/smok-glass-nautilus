# Changelog

## Unreleased

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
