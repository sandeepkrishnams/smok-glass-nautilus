# Security

This is experimental software that previews untrusted local files. The current maintained development line is 0.1.x, based on Nautilus 50.2.2; there is no independent long-term security-support promise.

Do not include sensitive document contents or exploitable samples in public issues. Report vulnerabilities through this repository's GitHub private vulnerability reporting when enabled. If private reporting is unavailable, ask the maintainer for a private reporting channel without publishing exploit details.

The preview layer uses read-only extraction/rendering, bounded inputs, disabled spreadsheet JavaScript, escaped HTML, and isolated LibreOffice rendering. These measures do not replace updates to Nautilus, GTK, WebKit, Poppler, LibreOffice, Python, or the operating system.

Maintainers should track upstream Nautilus and dependency fixes, and enable GitHub private vulnerability reporting before a public release.
