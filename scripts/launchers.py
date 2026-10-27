#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Per-user launchers with backups; the distribution binary stays untouched."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess

HOME = Path.home()
STATE = Path(os.environ.get('XDG_STATE_HOME', HOME / '.local/state')) / 'smok-glass-nautilus'
MARKER = '# Managed by smok-glass-nautilus'


def desktop_quote(path):
    return '"' + str(path).replace('\\','\\\\').replace('"','\\"').replace('%','%%') + '"'


def install(prefix, activate):
    binary = prefix / 'bin/nautilus'
    if not binary.is_file():
        raise SystemExit('Install the Meson build before creating launchers.')
    backup = STATE / ('backup-' + datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f'))
    backup.mkdir(parents=True)
    tracked = {}
    def write(path, contents, executable=False):
        key = str(path.relative_to(HOME))
        tracked[key] = path.exists()
        if path.exists():
            target = backup / key
            target.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(path,target)
        path.parent.mkdir(parents=True,exist_ok=True)
        path.write_text(contents)
        if executable:
            path.chmod(0o755)
    wrapper = HOME / '.local/bin/smok-glass-nautilus'
    write(wrapper, '#!/usr/bin/env bash\n' + MARKER + '\nset -euo pipefail\nexport GSETTINGS_SCHEMA_DIR=' +
          shlex.quote(str(prefix / 'share/glib-2.0/schemas')) + '\nexec ' + shlex.quote(str(binary)) + ' "$@"\n', True)
    old_default = subprocess.check_output(['xdg-mime','query','default','inode/directory'],text=True).strip()
    if activate:
        desktop = Path('/usr/share/applications/org.gnome.Nautilus.desktop').read_text()
        desktop = re.sub(r'^Exec=(?:/usr/bin/)?nautilus\b',lambda _: 'Exec=' + desktop_quote(wrapper),desktop,flags=re.M)
        write(HOME / '.local/share/applications/org.gnome.Nautilus.desktop', MARKER + '\n' + desktop)
        for name in ['org.gnome.Nautilus','org.freedesktop.FileManager1']:
            write(HOME / '.local/share/dbus-1/services' / (name + '.service'),
                  MARKER + '\n[D-BUS Service]\nName=' + name + '\nExec=' + shlex.quote(str(wrapper)) + ' --gapplication-service\n')
        subprocess.run(['update-desktop-database',str(HOME/'.local/share/applications')],check=True)
        subprocess.run(['xdg-mime','default','org.gnome.Nautilus.desktop','inode/directory'],check=True)
    # Keep existing extension modules available through Nautilus's stable API.
    libdir = Path(subprocess.check_output(['pkg-config','--variable=libdir','gtk4'],text=True).strip())
    modules = libdir / 'nautilus/extensions-4'
    target = prefix / 'lib' / 'nautilus/extensions-4'
    if (prefix/'lib/x86_64-linux-gnu').exists():
        target = prefix/'lib/x86_64-linux-gnu/nautilus/extensions-4'
    if modules.is_dir() and not target.exists():
        target.parent.mkdir(parents=True,exist_ok=True)
        target.symlink_to(modules,target_is_directory=True)
    (backup/'state.json').write_text(json.dumps({'files':tracked,'prefix':str(prefix),'default':old_default},indent=2))
    (STATE/'latest').write_text(str(backup))
    print('Installed launcher:',wrapper)
    print('Backup:',backup)


def uninstall(stock):
    latest = STATE/'latest'
    if not latest.exists():
        raise SystemExit('No launcher installation record was found.')
    backup = Path(latest.read_text().strip())
    state = json.loads((backup/'state.json').read_text())
    for relative, existed in state['files'].items():
        path = HOME/relative
        if path.exists() and MARKER not in path.read_text(errors='replace'):
            print('Preserving a launcher edited by another tool:',path)
            continue
        if existed and not stock:
            shutil.copy2(backup/relative,path)
        elif path.exists():
            path.unlink()
    subprocess.run(['update-desktop-database',str(HOME/'.local/share/applications')],check=True)
    default = 'org.gnome.Nautilus.desktop' if stock else state['default']
    if default:
        subprocess.run(['xdg-mime','default',default,'inode/directory'],check=True)
    print('Launchers restored. Restart Files when idle. Installed build retained at:',state['prefix'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('action',choices=['install','uninstall'])
    parser.add_argument('--prefix',type=Path)
    parser.add_argument('--no-activate',action='store_true')
    parser.add_argument('--stock',action='store_true')
    args = parser.parse_args()
    if args.action == 'install':
        if args.prefix is None: parser.error('--prefix is required for install')
        install(args.prefix.resolve(),not args.no_activate)
    else:
        uninstall(args.stock)
