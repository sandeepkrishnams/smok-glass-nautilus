#!/usr/bin/env bash
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${BUILD_DIR:-"$project_dir/build"}
export SMOK_GLASS_TEST_DATA="$project_dir/samples"
export SMOK_GLASS_TEST_OUTPUT="$project_dir/test-artifacts"
export GSETTINGS_SCHEMA_DIR="$build_dir/data"
export GSETTINGS_BACKEND=memory
export G_DEBUG=fatal-criticals
"$build_dir/src/nautilus-preview-check" "$@"
