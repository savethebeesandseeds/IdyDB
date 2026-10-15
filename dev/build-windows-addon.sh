#!/bin/sh
# Cross compile in idydb-dev after setup-mingw.sh and setup-windows-node.sh.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
compiler=x86_64-w64-mingw32-g++-posix
dependencies=/build/windows-node-deps
test -f "$dependencies/node-v24.19.0/include/node/node_api.h"
test -f "$dependencies/downloads/node.lib"
test -f "$dependencies/npm/node_modules/node-addon-api/napi.h"
test -f /build/windows-deps/lib/libcrypto.a
run_id="run-$(date -u +%Y%m%dT%H%M%SZ)-$$"
payload="/build/windows-addon-runtime/$run_id"
mkdir -p "$payload"
cp "$repo/LICENSE" "$payload/IdyDB-LICENSE.txt"
cp /build/windows-deps/share/openssl/copyright "$payload/OpenSSL-copyright.txt"
cp "$dependencies/npm/node_modules/node-addon-api/LICENSE.md" "$payload/node-addon-api-LICENSE.txt"
for package in gcc-mingw-w64-x86-64-posix-runtime mingw-w64-common; do
    cp "/usr/share/doc/$package/copyright" "$payload/$package-copyright.txt"
done
for configuration in production instrumented; do
    destination="$payload/$configuration"
    mkdir -p "$destination/build/Release"
    testing=''
    if [ "$configuration" = instrumented ]; then testing=-DIDYDB_TESTING=1; fi
    "$compiler" -shared -std=c++17 -O2 -Wall -Wextra -Werror \
        -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DNAPI_VERSION=8 \
        -DNODE_GYP_MODULE_NAME=idydb -DBUILDING_NODE_EXTENSION \
        -DNODE_ADDON_API_CPP_EXCEPTIONS -DNODE_ADDON_API_CPP_EXCEPTIONS_ALL $testing \
        -isystem "$dependencies/node-v24.19.0/include/node" \
        -isystem "$dependencies/npm/node_modules/node-addon-api" \
        -I "$repo/idydb/include" -I /build/windows-deps/include \
        "$repo/addons/addon.cpp" "$repo/idydb/impl/db.cpp" "$repo/idydb/impl/platform_windows.cpp" \
        "$dependencies/downloads/node.lib" /build/windows-deps/lib/libcrypto.a \
        -ladvapi32 -lws2_32 -lcrypt32 -lgdi32 \
        -o "$destination/build/Release/idydb.node"
    cp "$repo/addons/test.js" "$repo/addons/test-failure.js" "$destination/"
    for library in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
        runtime=$("$compiler" -print-file-name="$library")
        test "$runtime" != "$library"
        test -f "$runtime"
        cp "$runtime" "$destination/build/Release/"
    done
done
cat > "$payload/manifest.json" <<EOF
{"schema":1,"run":"$run_id","node":"24.19.0","architecture":"x64","napi":8}
EOF
cp "$payload/manifest.json" /build/windows-addon-runtime/latest.json
printf '\nWindows Node-API runtime payload ready: %s\n' "$payload"
