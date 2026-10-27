#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
if [[ ! -f /etc/debian_version ]]; then
  printf 'This dependency installer targets Ubuntu 26.04. See README.md for the required libraries.\n' >&2
  exit 1
fi
admin=()
if [[ $(id -u) -ne 0 ]]; then admin=(sudo); fi
"${admin[@]}" apt-get update
"${admin[@]}" apt-get install --no-install-recommends -y \
  build-essential git meson ninja-build pkg-config gettext desktop-file-utils \
  shared-mime-info blueprint-compiler libglib2.0-dev libgtk-4-dev libadwaita-1-dev \
  libglycin-2-dev libglycin-gtk4-2-dev libgnome-autoar-0-dev libgnome-desktop-4-dev \
  libportal-dev libportal-gtk4-dev libtinysparql-dev libicu-dev libselinux1-dev \
  libwayland-dev libx11-dev libcmark-dev libwebkitgtk-6.0-dev \
  python3 python3-openpyxl poppler-utils libreoffice-writer libreoffice-calc bubblewrap
