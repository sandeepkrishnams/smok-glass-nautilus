# Smok Glass Nautilus

A Nautilus file manager with a built-in, read-only preview pane. Select a file or folder once to see it on the right: folder contents, formatted document pages, a spreadsheet grid, images, Markdown, and plain text.

Based on **GNOME Nautilus 50.2.2**. This repository contains the complete modified source, local document readers, sample files, and per-user installation tools.

## See it

### Spreadsheet preview

Column letters, row numbers, worksheet tabs, merged cells, colours, borders, and common number formats—all inside the preview pane.

![Read-only spreadsheet preview showing a styled workbook with worksheet tabs](docs/screenshots/spreadsheet-preview.png)

### Folder preview

Select a folder to see its immediate files and subfolders, with folders first, file icons, sizes, and an item summary. **Ctrl+H** also updates hidden items in the preview.

<img src="docs/screenshots/folder-preview.png" alt="A folder preview listing subfolders, files and a shortcut" width="390">

### Document preview

Word documents render as pages with their layout preserved. Use the arrows to browse pages.

<img src="docs/screenshots/document-preview.png" alt="A formatted Word document in the Nautilus preview pane, with page controls" width="390">

These are real captures from the native preview widget using the sample files in this repository.

## Features

- Embedded, resizable right preview pane; **Ctrl+Shift+P** toggles it.
- Single-click and arrow-key selection update the preview. Double-click opens the file normally.
- Folder contents with folders first, file icons and sizes, hidden-item preference, and empty-folder states.
- Rendered Markdown with a source toggle; scrollable, selectable plain text.
- Image previews through GNOME's sandboxed Glycin loaders.
- Formatted PDF, DOCX, ODT, DOC, and RTF pages with Previous/Next controls.
- XLSX, XLS, ODS, CSV, and TSV previews in a read-only spreadsheet grid with worksheet tabs.
- Background loading, rapid-selection debounce, cancellation, and stale-result protection.
- Per-user installation with launcher backups. The distribution's `/usr/bin/nautilus` stays intact.

## Status and requirements

**Experimental desktop software.** Tested locally on **Ubuntu 26.04 / GNOME 50**, GTK 4.22 and libadwaita 1.9. The source requires GTK ≥4.20, libadwaita ≥1.8, GLib ≥2.84, Glycin 2, and the other libraries listed in `meson.build`.

The `smok-glass` helpers also require Python 3, openpyxl, Poppler, LibreOffice Writer/Calc, Bubblewrap, and WebKitGTK 6. The dependency installer currently targets Ubuntu 26.04; other distributions need equivalent packages and compatible library versions.

## Build and install

Clone this repository, then run:

```bash
git clone https://github.com/sandeepkrishnams/smok-glass-nautilus.git
cd smok-glass-nautilus
./scripts/install-deps.sh
./scripts/build.sh
./scripts/test.sh
./scripts/install.sh
```

The default prefix is `~/.local/opt/smok-glass-nautilus`. Only dependency installation uses administrator access; the build and file-manager launchers install under your own home directory.

After active file transfers finish, close Files, run `nautilus --quit`, and reopen Files. The normal Files launcher will use this build. Open `samples/` to try the previews.

To install the binary without changing your default Files launcher:

```bash
./scripts/install.sh --no-activate
~/.local/bin/smok-glass-nautilus
```

To choose another prefix, set `PREFIX` before building:

```bash
PREFIX="$HOME/.local/opt/my-nautilus" ./scripts/build.sh
./scripts/install.sh
```

Existing system Nautilus extension modules are reused where available. The default build disables compiling upstream stock extensions and cloud-provider integration to keep its dependency set manageable.

## Controls

| Action | Control |
| --- | --- |
| Show/hide preview | **Ctrl+Shift+P**, or View menu → Preview Pane |
| Change preview | Click a file or folder, or use **↑ / ↓** in the file list |
| Resize pane | Drag its divider |
| Browse document pages | Previous/Next buttons beneath the page |
| Change worksheet | Tabs beneath the spreadsheet grid |
| Read Markdown source | **Show Markdown source** |
| Edit/open a file | Double-click it or use **Open in application** |

## Preview limits

- File-content previews support local files only; unsupported and remote files offer the normal Open action. Folder listings also support locations available through GIO, including mounted network folders.
- Folder previews list immediate children only, with up to 500 visible items and 10,000 entries scanned. Limited previews say so; use **Open folder** to browse the full listing. Symlinks inside a folder are listed without following their targets.
- Text previews stop after 512 KiB. Document and image input files above 100 MiB show a fallback.
- Spreadsheet previews show up to 5 visible sheets, 200 rows and 30 columns per sheet.
- Spreadsheet formulas are not recalculated. Saved results are shown; `—` marks a formula without a saved result. Charts, embedded drawings, conditional formatting, and every Excel number-format variant are not implemented.
- Word/OpenDocument pages use LibreOffice rendering. Available fonts can affect layout compared with Google Docs or Microsoft Office.
- Encrypted, damaged, unusually complex, and timed-out documents show an explanation instead of a preview.

Office conversions run headlessly with networking disabled, a read-only source filesystem, and macros disabled in a private profile. Spreadsheet content is escaped; its viewer disables JavaScript and uses a restrictive content policy. Previews do not edit the original files.

Converted office previews are stored in a private cache under `${XDG_CACHE_HOME:-~/.cache}/smok-glass-nautilus`, keyed by source path, modification time and size. The cache retains at most 32 items / 128 MiB; deleting it clears the previews without affecting original documents.

## Optional glass appearance

The preview feature works with your normal GTK theme. A charcoal glass stylesheet is included in `styles/gtk4.css`.

Back up `~/.config/gtk-4.0/gtk.css`, then add an `@import` pointing to that stylesheet if you want the matching appearance. Real background blur additionally requires a compatible [Blur My Shell](https://github.com/aunetx/blur-my-shell) installation, application blur enabled for `org.gnome.Nautilus`, and suitable transparent surfaces. The project does not automatically install or change GNOME Shell extensions.

## Restore your previous file manager

```bash
./scripts/uninstall.sh
```

This restores backed-up launchers. To remove this project's launcher overrides and explicitly use the distribution's Nautilus instead:

```bash
./scripts/uninstall.sh --stock
```

Restart Files after switching. The installed build is retained so it can be inspected or reactivated; its location is printed by the script.

## Development and tests

```bash
./scripts/test.sh
./scripts/test-native.sh
./scripts/test-native.sh --sheet-only
```

Native widget checks require a graphical session and the helpers installed at the build's prefix. They exercise Markdown, text, images, formatted office pages, PDF navigation, spreadsheet rendering, cancellation, and large-file handling. Captures go into ignored `test-artifacts/`.

See [CONTRIBUTING.md](CONTRIBUTING.md), [SECURITY.md](SECURITY.md), and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Upstream and licence

Smok Glass Nautilus is a community modification of [GNOME Nautilus](https://gitlab.gnome.org/GNOME/nautilus), starting from tag `50.2.2`. Original copyright notices and file-level licences are retained. The original project documentation is preserved in [README.upstream.md](README.upstream.md).

The project is distributed under **GPL-3.0-or-later**; see [LICENSE](LICENSE). Some upstream components retain their own compatible file-level licences. Runtime dependencies retain their respective licences and are not vendored here.

This fork does not automatically receive later Nautilus fixes. Maintainers and users should track upstream security updates and rebuild or restore the distribution version when needed.
