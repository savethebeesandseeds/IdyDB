#!/bin/sh
# Dependencies only. Container lifecycle and project commands belong in dev.ps1.
set -eu
export DEBIAN_FRONTEND=noninteractive
workspace=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
apt-get update
if [ ! -s "$workspace/dev/packages.lock" ]; then
    echo 'Missing dev/packages.lock; resolve the dependency versions before setup.' >&2
    exit 1
fi
set --
while IFS= read -r package; do
    case "$package" in ''|'#'*) continue ;; esac
    set -- "$@" "$package"
done < "$workspace/dev/packages.lock"
apt-get install -y --no-install-recommends "$@"
