#!/usr/bin/env sh
set -eu

cd "$(dirname "$0")/.."

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure

echo
echo "Done. Run it with: sudo ./build/netpulse 127.0.0.1"
echo "To run it without sudo, see scripts/install.sh or the README."
