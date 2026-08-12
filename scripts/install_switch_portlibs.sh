#!/bin/sh
# Install Switch portlibs for CI/devkitPro container builds.
#
# pkg.devkitpro.org sits behind Cloudflare and intermittently 403s shared CI
# runner IPs. Prefer cached pacman databases when present, then retry sync
# with exponential backoff.

set -eu

PACKAGES="switch-dev switch-curl switch-mbedtls switch-zlib switch-miniupnpc"

if dkp-pacman -Q --noconfirm ${PACKAGES}; then
    echo "Switch portlibs already installed."
    exit 0
fi

# Cached databases from actions/cache may be fresh enough to install without
# hitting pkg.devkitpro.org for the .db files.
if dkp-pacman -S --noconfirm --needed ${PACKAGES}; then
    exit 0
fi

attempt=1
while [ "${attempt}" -le 10 ]; do
    if dkp-pacman -Sy --noconfirm --needed ${PACKAGES}; then
        exit 0
    fi
    echo "dkp-pacman attempt ${attempt} failed, retrying..." >&2
    sleep $((attempt * 15))
    attempt=$((attempt + 1))
done

exit 1
