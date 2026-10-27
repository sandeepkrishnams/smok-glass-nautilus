#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${BUILD_DIR:-"$project_dir/build"}
if [[ ! -f "$build_dir/build.ninja" ]]; then "$project_dir/scripts/build.sh"; fi
meson install -C "$build_dir"
install_prefix=$(meson introspect "$build_dir" --buildoptions | /usr/bin/python3 -c 'import json,sys; print(next(x["value"] for x in json.load(sys.stdin) if x["name"]=="prefix"))')
/usr/bin/python3 "$project_dir/scripts/launchers.py" install --prefix "$install_prefix" "$@"
printf '\nInstalled. Close Files after transfers finish, run nautilus --quit, then reopen Files.\n'
