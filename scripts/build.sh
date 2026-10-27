#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
install_prefix=${PREFIX:-"$HOME/.local/opt/smok-glass-nautilus"}
build_dir=${BUILD_DIR:-"$project_dir/build"}
setup_flags=()
if [[ -f "$build_dir/build.ninja" ]]; then setup_flags=(--reconfigure); fi
meson setup "${setup_flags[@]}" "$build_dir" "$project_dir" --prefix="$install_prefix" \
  --buildtype=debugoptimized -Dextensions=false -Dcloudproviders=disabled \
  -Dselinux=enabled -Dintrospection=false -Dtests=headless
meson compile -C "$build_dir" -j "${JOBS:-4}"
