#!/bin/bash
# Build globecore-desktop in the VS2019 x64 MSVC environment (incremental).
# Assumes the project is already configured (see gcd-rebuild.sh for a fresh configure).
source "$(dirname "${BASH_SOURCE[0]}")/vs2019-env.sh"
source "$(dirname "${BASH_SOURCE[0]}")/gcd-paths.sh"
vcvars_import || { echo "vcvars failed"; exit 2; }
"$CMAKE" --build "$BUILD" -j 8
echo "=== BUILD EXIT $? ==="
