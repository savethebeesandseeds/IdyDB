#!/bin/sh
# Dependency setup only, inside idydb-dev. Never downloads a Windows runtime.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
root=/build/windows-node-deps
version=24.19.0
headers="node-v$version-headers.tar.gz"
headers_sha=54f14a297d47ea0794fe272363703d9dc419c96ac68f20d890f98b63754a3e4c
import_sha=63ec831bbf164d1b23197d6fac1944dfb146534e332889ca0755d250e8dedff9
mkdir -p "$root/downloads" "$root/npm"
download() {
    if [ ! -f "$2" ]; then curl --fail --location --retry 3 "$1" --output "$2"; fi
}
download "https://nodejs.org/dist/v$version/SHASUMS256.txt" "$root/downloads/SHASUMS256.txt"
grep -Fx "$headers_sha  $headers" "$root/downloads/SHASUMS256.txt" >/dev/null
grep -Fx "$import_sha  win-x64/node.lib" "$root/downloads/SHASUMS256.txt" >/dev/null
download "https://nodejs.org/dist/v$version/$headers" "$root/downloads/$headers"
download "https://nodejs.org/dist/v$version/win-x64/node.lib" "$root/downloads/node.lib"
printf '%s  %s\n%s  %s\n' "$headers_sha" "$root/downloads/$headers" \
    "$import_sha" "$root/downloads/node.lib" | sha256sum --check --strict
if [ ! -f "$root/node-v$version/include/node/node_api.h" ]; then
    tar -xzf "$root/downloads/$headers" -C "$root"
fi
cp "$repo/addons/package.json" "$repo/addons/package-lock.json" "$root/npm/"
cd "$root/npm"
npm ci --ignore-scripts --cache /build/npm-cache
printf '\nWindows Node-API dependencies ready: Node %s headers/import library; node-addon-api 8.5.0.\n' "$version"
