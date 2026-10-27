#!/usr/bin/env bash
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
/usr/bin/python3 -m unittest discover -s "$project_dir/tests" -v
meson test -C "${BUILD_DIR:-$project_dir/build}" --print-errorlogs \
  test-file test-filename-utilities test-ui-utilities
