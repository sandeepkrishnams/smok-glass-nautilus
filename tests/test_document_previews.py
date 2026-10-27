# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'smok-glass'))
import formatted_previews as formatted
spec = importlib.util.spec_from_file_location('document_helper',ROOT/'smok-glass/preview-documents.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


class DocumentPreviewTests(unittest.TestCase):
    def test_formatted_sheet_grid(self):
        html = formatted.spreadsheet(ROOT/'samples/sheet.xlsx','xlsx','')
        for expected in ['<thead>', 'scope="row"', 'Overview</label>', 'Notes</label>',
                         'colspan="3"', '#188038', '$120.00']:
            self.assertIn(expected,html)
        self.assertNotIn('<script',html)
        self.assertNotIn('contenteditable',html)

    def test_cell_markup_is_escaped(self):
        from openpyxl import Workbook
        book = Workbook()
        book.active['A1'] = '<img src="https://example.com" onerror="alert(1)">'
        html = formatted.grid_html(book,book)
        self.assertIn('&lt;img',html)
        self.assertNotIn('<img',html)
        self.assertIn("connect-src 'none'",html)

    def test_delimited_tables(self):
        for name,kind in [('sheet.csv','csv'),('sheet.tsv','tsv')]:
            html = formatted.spreadsheet(ROOT/'samples'/name,kind,'')
            self.assertIn('Notebook',html)
            self.assertIn('<table>',html)

    def test_pdf_second_page(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(helper.pdf(ROOT/'samples/document.pdf',2,directory),2)
            self.assertTrue((Path(directory)/'page.png').read_bytes().startswith(b'\x89PNG'))

    def test_corrupt_archive(self):
        with tempfile.NamedTemporaryFile(suffix='.xlsx') as file:
            file.write(b'not a workbook');file.flush()
            with self.assertRaises(zipfile.BadZipFile):
                formatted.validate_workbook(file.name)

    def test_oversized_xml_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'large.xlsx'
            with zipfile.ZipFile(path,'w',zipfile.ZIP_DEFLATED) as archive:
                archive.writestr('xl/workbook.xml',b'X'*(8*1024*1024+1))
            with self.assertRaisesRegex(ValueError,'too large'):
                formatted.validate_workbook(path)

    def test_xml_entities_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'entities.xlsx'
            with zipfile.ZipFile(path,'w') as archive:
                archive.writestr('xl/workbook.xml',b'<!DOCTYPE x [<!ENTITY a "expanded">]><x>&a;</x>')
            with self.assertRaisesRegex(ValueError,'entities'):
                formatted.validate_workbook(path)


if __name__ == '__main__':
    unittest.main()
