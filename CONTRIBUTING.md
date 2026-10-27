# Contributing

Smok Glass Nautilus is an experimental fork based on GNOME Nautilus 50.2.2. Contributions should preserve normal file-manager behaviour and keep previews read-only.

1. Open an issue describing the problem or proposed change.
2. Keep changes focused. Avoid unrelated changes to upstream Nautilus code.
3. Run `./scripts/test.sh`. Preview/UI changes should also pass `./scripts/test-native.sh` in a graphical session.
4. Include a small, non-sensitive fixture or clear reproduction steps. Never upload private documents.
5. Include before/after captures for visual changes and describe the tested GNOME/GTK versions.

Use GPL-3.0-or-later for new project code and preserve upstream copyright and licence headers. Runtime dependencies should be declared rather than copied into this repository.

Document parsers must bound input/output sizes, avoid executing document content, escape spreadsheet cell text, and support cancellation. Do not enable JavaScript or network loading in the spreadsheet viewer.

Upstream source changes should be kept easy to compare with the pinned base release. See [README.upstream.md](README.upstream.md) and [CONTRIBUTING.upstream.md](CONTRIBUTING.upstream.md) for original GNOME context.
