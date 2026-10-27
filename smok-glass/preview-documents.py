#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Local read-only document rendering helper for Smok Glass Nautilus."""
from pathlib import Path
from xml.etree import ElementTree as ET
import csv
import os
import re
import subprocess
import sys
import zipfile


def pdf(path, page, directory):
    info = subprocess.run(['/usr/bin/pdfinfo', str(path)], capture_output=True, timeout=8,
                          env=dict(os.environ, LC_ALL='C'))
    if info.returncode:
        raise ValueError('PDF preview unavailable. The file may be encrypted or damaged.')
    match = re.search(rb'^Pages:\s*(\d+)', info.stdout, re.M)
    if not match:
        raise ValueError('Could not determine PDF page count.')
    pages = int(match.group(1))
    page = max(1, min(page, pages))
    output = Path(directory) / 'page'
    rendered = subprocess.run(['/usr/bin/pdftoppm', '-f', str(page), '-l', str(page),
                               '-singlefile', '-scale-to', '1400', '-png', str(path), str(output)],
                              capture_output=True, timeout=15)
    if rendered.returncode or not output.with_suffix('.png').exists():
        raise ValueError('This PDF page could not be rendered.')
    return pages


def main():
    if len(sys.argv) != 5:
        raise ValueError('Usage: preview-documents.py TYPE FILE PAGE WORK_DIRECTORY')
    kind, raw_path, page, directory = sys.argv[1:]
    path = Path(raw_path)
    if not path.is_file() or path.stat().st_size > 100 * 1024 * 1024:
        raise ValueError('Document is unavailable or larger than the 100 MiB preview limit.')
    if kind in ('pdf', 'docx', 'odt', 'doc', 'rtf'):
        from formatted_previews import convert_office
        if kind != 'pdf':
            path = convert_office(path, directory, 'pdf', 'writer_pdf_Export')
        print(pdf(path, int(page), directory))
    elif kind in ('xlsx', 'xls', 'ods', 'csv', 'tsv'):
        from formatted_previews import spreadsheet
        print(spreadsheet(path, kind, directory))
    else:
        raise ValueError('This document type is not supported.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, KeyError, ET.ParseError, zipfile.BadZipFile, RuntimeError,
            subprocess.TimeoutExpired, csv.Error) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
