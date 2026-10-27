# SPDX-License-Identifier: GPL-3.0-or-later
"""Formatted office previews: isolated page rendering and a read-only sheet grid."""
from pathlib import Path
from datetime import date, datetime
from html import escape
import csv
import io
import math
import os
import re
import subprocess
import sys
import zipfile
import hashlib
import shutil
import tempfile

sys.path.insert(0, str(Path(__file__).parent / 'python'))


def convert_office(path, directory, extension, export_filter):
    directory = Path(directory).resolve()
    output = directory / (Path(path).stem + '.' + extension)
    stat = Path(path).stat()
    identity = f'{Path(path).resolve()}:{stat.st_mtime_ns}:{stat.st_size}:{extension}:{export_filter}:v1'
    cache_root = Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache')) / 'smok-glass-nautilus'
    cache_root.mkdir(mode=0o700, parents=True, exist_ok=True)
    cache_file = cache_root / (hashlib.sha256(identity.encode()).hexdigest() + '.' + extension)
    if cache_file.is_file():
        try:
            shutil.copyfile(cache_file, output)
            return output
        except FileNotFoundError:
            pass
    profile = directory / 'office-profile'
    (profile / 'user').mkdir(parents=True, exist_ok=True)
    runtime = directory / 'runtime'
    runtime.mkdir(mode=0o700, exist_ok=True)
    cache = directory / 'cache'
    cache.mkdir(exist_ok=True)
    (profile / 'user/registrymodifications.xcu').write_text('''<?xml version="1.0" encoding="UTF-8"?>
<oor:items xmlns:oor="http://openoffice.org/2001/registry">
<item oor:path="/org.openoffice.Office.Common/Security/Scripting"><prop oor:name="MacroSecurityLevel" oor:op="fuse"><value>3</value></prop></item>
<item oor:path="/org.openoffice.Office.Common/Security/Scripting"><prop oor:name="DisableMacrosExecution" oor:op="fuse"><value>true</value></prop></item>
<item oor:path="/org.openoffice.Office.Common/Load"><prop oor:name="UpdateDocMode" oor:op="fuse"><value>0</value></prop></item>
</oor:items>''', encoding='utf-8')
    command = ['/usr/bin/bwrap', '--unshare-net', '--die-with-parent', '--ro-bind', '/', '/',
               '--dev-bind', '/dev', '/dev', '--proc', '/proc', '--tmpfs', '/tmp',
               '--bind', str(directory), str(directory),
               '--unsetenv', 'DBUS_SESSION_BUS_ADDRESS',
               '--unsetenv', 'G_DEBUG', '--unsetenv', 'GSETTINGS_SCHEMA_DIR',
               '--setenv', 'XDG_RUNTIME_DIR', str(runtime),
               '--setenv', 'XDG_CACHE_HOME', str(cache),
               '--setenv', 'GSETTINGS_BACKEND', 'memory',
               '--setenv', 'SAL_USE_VCLPLUGIN', 'svp',
               '/usr/bin/libreoffice', '-env:UserInstallation=' + profile.as_uri(),
               '--headless', '--nologo', '--nodefault', '--norestore', '--nolockcheck',
               '--convert-to', extension + ':' + export_filter, '--outdir', str(directory), str(path)]
    result = subprocess.run(command, capture_output=True, timeout=35,
                            env=dict(os.environ, LC_ALL='C.UTF-8'))
    if result.returncode or not output.exists():
        log = Path(os.environ.get('XDG_STATE_HOME', Path.home()/'.local/state')) / 'smok-glass-nautilus/office-render-errors.log'
        try:
            log.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            log.write_text(result.stdout.decode('utf-8', errors='replace') + '\n' + result.stderr.decode('utf-8', errors='replace'))
        except OSError:
            pass
        raise ValueError('Formatted preview could not be created. The document may be encrypted or damaged.')
    if output.stat().st_size <= 40*1024*1024:
        with tempfile.NamedTemporaryFile(dir=cache_root, prefix='pending-', delete=False) as pending:
            pending_path = Path(pending.name)
            with output.open('rb') as source:
                shutil.copyfileobj(source,pending)
        pending_path.replace(cache_file)
        entries = []
        for item in cache_root.iterdir():
            if item.suffix not in ('.pdf','.xlsx','.ods','.odt'):
                continue
            try: entries.append((item,item.stat()))
            except FileNotFoundError: pass
        entries.sort(key=lambda pair: pair[1].st_mtime, reverse=True)
        total = 0
        for index,(item,entry_stat) in enumerate(entries):
            total += entry_stat.st_size
            if index >= 32 or total > 128*1024*1024:
                item.unlink(missing_ok=True)
    return output


def validate_workbook(path):
    total = 0
    with zipfile.ZipFile(path) as archive:
        if len(archive.infolist()) > 10000:
            raise ValueError('Workbook is too complex for a quick preview.')
        for info in archive.infolist():
            if not info.filename.endswith(('.xml', '.rels')):
                continue
            total += info.file_size
            if info.file_size > 8 * 1024 * 1024 or total > 32 * 1024 * 1024:
                raise ValueError('Workbook is too large for a quick preview. Open it in its application.')
            data = archive.read(info)
            if b'<!DOCTYPE' in data or b'<!ENTITY' in data or b'\0' in data:
                raise ValueError('Workbook XML encoding or entities are not supported in previews.')


def color(value, fallback):
    if value is None:
        return fallback
    if value.type == 'rgb':
        raw = str(value.rgb)[-6:]
        return '#' + raw if re.fullmatch('[0-9a-fA-F]{6}', raw) else fallback
    if value.type == 'theme':
        # Office's default palette, including tint variations.
        colors = ['#ffffff', '#111111', '#e7e6e6', '#44546a', '#4472c4', '#ed7d31', '#a5a5a5', '#ffc000', '#5b9bd5', '#70ad47']
        index = int(value.theme)
        base = colors[index] if 0 <= index < len(colors) else fallback
        tint = float(value.tint or 0)
        if tint and re.fullmatch('#[0-9a-fA-F]{6}', base):
            rgb = [int(base[i:i+2], 16) for i in (1, 3, 5)]
            rgb = [round(x * (1 + tint) if tint < 0 else x + (255 - x) * tint) for x in rgb]
            return '#' + ''.join(f'{max(0,min(255,x)):02x}' for x in rgb)
        return base
    return fallback


def cell_style(cell):
    font, alignment = cell.font, cell.alignment
    family = re.sub(r'[^a-zA-Z0-9 _-]', '', str(font.name or 'Arial'))[:80]
    rules = [f'font-family:{family},Arial,sans-serif', f'font-size:{min(36,max(8,float(font.sz or 11)))}pt',
             'color:' + color(font.color, '#202124')]
    if font.bold:
        rules.append('font-weight:700')
    if font.italic:
        rules.append('font-style:italic')
    if font.underline:
        rules.append('text-decoration:underline')
    if cell.fill.patternType == 'solid':
        rules.append('background:' + color(cell.fill.fgColor, '#ffffff'))
    horizontal = alignment.horizontal
    if horizontal in ('left', 'center', 'right', 'justify'):
        rules.append('text-align:' + horizontal)
    elif cell.data_type == 'n' and cell.value is not None:
        rules.append('text-align:right')
    if alignment.wrap_text:
        rules.extend(['white-space:pre-wrap', 'overflow-wrap:anywhere'])
    if alignment.vertical in ('top', 'center', 'bottom'):
        rules.append('vertical-align:' + ('middle' if alignment.vertical == 'center' else alignment.vertical))
    for edge in ['top', 'right', 'bottom', 'left']:
        border = getattr(cell.border, edge)
        if border and border.style:
            width = 3 if border.style in ('thick', 'double') else 2 if border.style == 'medium' else 1
            style = 'dashed' if 'dash' in border.style.lower() else 'solid'
            rules.append(f'border-{edge}:{width}px {style} ' + color(border.color, '#666666'))
    return ';'.join(rules)


def display_value(cell, saved):
    value = saved.value if cell.data_type == 'f' else cell.value
    if value is None and cell.data_type == 'f':
        return '—'
    if value is None:
        return ''
    if isinstance(value, (date, datetime)):
        if isinstance(value, datetime) and (value.hour or value.minute or value.second):
            return value.strftime('%Y-%m-%d %H:%M')
        return value.strftime('%Y-%m-%d')
    if isinstance(value, bool):
        return 'TRUE' if value else 'FALSE'
    if isinstance(value, (float, int)) and math.isfinite(value):
        fmt = cell.number_format or 'General'
        if fmt == 'General':
            return f'{value:g}'
        decimals = len(re.search(r'\.([0#]+)', fmt).group(1)) if re.search(r'\.([0#]+)', fmt) else 0
        if '%' in fmt:
            return f'{value * 100:.{min(decimals,8)}f}%'
        numeric = f'{value:,.{min(decimals,8)}f}' if ',' in fmt else f'{value:.{min(decimals,8)}f}'
        symbol = next((symbol for symbol in ['$','€','£','₹'] if symbol in fmt), '')
        return symbol + numeric
    return str(value)[:2000]


def grid_html(workbook, cached):
    from openpyxl.utils import get_column_letter
    sheets = [sheet for sheet in workbook.worksheets if sheet.sheet_state == 'visible'][:5]
    if not sheets:
        raise ValueError('Workbook has no visible worksheets.')
    style_map = {}
    grids, selectors, labels, visibility = [], [], [], []
    for index, sheet in enumerate(sheets):
        sid = f'sheet{index}'
        selectors.append(f'<input class="selector" type="radio" name="sheet" id="{sid}" {"checked" if index == 0 else ""}>')
        labels.append(f'<label for="{sid}">{escape(sheet.title)}</label>')
        visibility.append(f'#{sid}:checked~.sheets .{sid}' + '{display:block}' + f'#{sid}:checked~.tabs label[for="{sid}"]' + '{background:#e6f4ea;color:#137333;box-shadow:inset 0 -3px #188038}')
        rows, columns = min(200, max(30, sheet.max_row)), min(30, max(10, sheet.max_column))
        merges, covered = {}, set()
        for merge in sheet.merged_cells.ranges:
            if merge.min_row > rows or merge.min_col > columns:
                continue
            end_row, end_col = min(rows, merge.max_row), min(columns, merge.max_col)
            merges[(merge.min_row, merge.min_col)] = (end_row - merge.min_row + 1, end_col - merge.min_col + 1)
            for row in range(merge.min_row, end_row + 1):
                for col in range(merge.min_col, end_col + 1):
                    if (row, col) != (merge.min_row, merge.min_col):
                        covered.add((row, col))
        table = [f'<section class="sheet {sid}" aria-label="{escape(sheet.title,quote=True)}"><table><colgroup><col style="width:44px">']
        for col in range(1, columns + 1):
            width = sheet.column_dimensions[get_column_letter(col)].width
            table.append(f'<col style="width:{min(480,max(55,int((width or 13)*7+8)))}px">')
        table.append('</colgroup><thead><tr><th class="corner"></th>')
        table.extend(f'<th scope="col">{get_column_letter(col)}</th>' for col in range(1, columns + 1))
        table.append('</tr></thead><tbody>')
        for row in range(1, rows + 1):
            height = min(160, max(22, (sheet.row_dimensions[row].height or 15) * 1.34))
            table.append(f'<tr style="height:{height}px"><th scope="row">{row}</th>')
            for col in range(1, columns + 1):
                if (row,col) in covered:
                    continue
                cell = sheet.cell(row,col)
                key = cell.style_id
                if key not in style_map:
                    style_map[key] = cell_style(cell)
                value = display_value(cell, cached[sheet.title].cell(row,col))
                span = merges.get((row,col), (1,1))
                title = 'Formula result not saved: ' + str(cell.value) if cell.data_type == 'f' and cached[sheet.title].cell(row,col).value is None else value
                table.append(f'<td class="s{key}" rowspan="{span[0]}" colspan="{span[1]}" title="{escape(title,quote=True)}">{escape(value)}</td>')
            table.append('</tr>')
        table.append('</tbody></table></section>')
        grids.append(''.join(table))
    styles = ''.join(f'.s{key}'+'{'+value+'}' for key,value in style_map.items())
    return '''<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; script-src 'none'; connect-src 'none'; form-action 'none'; base-uri 'none'">
<style>
*{box-sizing:border-box}html,body{height:100%;margin:0;background:#fff;color:#202124;font:13px Arial,sans-serif}
body{display:flex;flex-direction:column;overflow:hidden}.selector{position:absolute;opacity:0;width:1px;height:1px}.sheets{flex:1;min-height:0}.sheet{display:none;height:100%;overflow:auto}
table{border-collapse:separate;border-spacing:0;table-layout:fixed;width:max-content;min-width:100%}td,th{border-right:1px solid #e2e5e9;border-bottom:1px solid #e2e5e9;padding:3px 6px;height:23px;overflow:hidden;white-space:nowrap;max-width:480px}
thead th{position:sticky;top:0;z-index:3;height:27px}th{background:#f8f9fa;color:#5f6368;font-size:12px;font-weight:400;text-align:center;user-select:none}tbody th{position:sticky;left:0;z-index:2;width:44px}.corner{left:0;z-index:4}
td{background:#fff;user-select:text}td:hover{box-shadow:inset 0 0 0 1px #1a73e8}.tabs{display:flex;flex-shrink:0;overflow-x:auto;background:#f8f9fa;border-top:1px solid #dadce0;min-height:39px}.tabs label{padding:11px 18px;white-space:nowrap;font-weight:600;cursor:pointer}.limit{flex-shrink:0;padding:5px 9px;background:#f8f9fa;color:#6b7280;font-size:11px;border-top:1px solid #e5e7eb}
''' + styles + ''.join(visibility) + '</style></head><body>' + ''.join(selectors) + '<div class="sheets">' + ''.join(grids) + '</div><div class="tabs" role="tablist" aria-label="Worksheets">' + ''.join(labels) + '</div><div class="limit">Read-only preview · up to 5 sheets, 200 rows × 30 columns · saved values</div></body></html>'


def spreadsheet(path, kind, directory):
    from openpyxl import Workbook, load_workbook
    if kind in ('csv','tsv'):
        with open(path,'rb') as file:
            raw = file.read(512*1024)
        text = raw.decode('utf-8-sig',errors='replace')
        delimiter = '\t' if kind == 'tsv' else ','
        if kind == 'csv':
            try: delimiter = csv.Sniffer().sniff(text[:8192],delimiters=',;\t').delimiter
            except csv.Error: pass
        workbook = Workbook()
        workbook.active.title = path.stem[:31]
        for index,row in enumerate(csv.reader(io.StringIO(text),delimiter=delimiter)):
            if index >= 200: break
            for col,value in enumerate(row[:30],1):
                cell = workbook.active.cell(index+1,col)
                cell.value = value
                cell.data_type = 's'
        cached = workbook
    else:
        if kind in ('ods','xls'):
            path = convert_office(path,directory,'xlsx','Calc MS Excel 2007 XML')
        validate_workbook(path)
        workbook = load_workbook(path,data_only=False,keep_links=False)
        cached = load_workbook(path,data_only=True,keep_links=False)
    html = grid_html(workbook,cached)
    if len(html.encode('utf-8')) > 4*1024*1024:
        raise ValueError('This formatted workbook is too complex for a quick preview.')
    return html
