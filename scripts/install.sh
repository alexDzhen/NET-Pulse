#!/usr/bin/env sh
# Builds netpulse if needed, installs it and lets it run without sudo.
# Usage: scripts/install.sh [prefix]     (default prefix: /usr/local)
set -eu

cd "$(dirname "$0")/.."
PREFIX="${1:-/usr/local}"

if [ ! -x build/netpulse ]; then
    ./scripts/build.sh
fi

sudo cmake --install build --prefix "$PREFIX"

if sudo setcap cap_net_raw+ep "$PREFIX/bin/netpulse"; then
    echo "Installed $PREFIX/bin/netpulse with the CAP_NET_RAW capability."
else
    echo "Installed $PREFIX/bin/netpulse, but the capability could not be set."
    echo "Install setcap (package libcap2-bin or libcap) and rerun, or run netpulse with sudo."
fi
