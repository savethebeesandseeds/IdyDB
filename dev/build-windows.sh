#!/bin/sh
# Run in idydb-dev after dev/setup-mingw.sh; never installs dependencies.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
compiler=x86_64-w64-mingw32-g++-posix
command -v "$compiler" >/dev/null
test -f /build/windows-deps/include/openssl/evp.h
test -f /build/windows-deps/lib/libcrypto.a
run_id="run-$(date -u +%Y%m%dT%H%M%SZ)-$$"
payload="/build/windows-runtime/$run_id"
mkdir -p "$payload/fixtures"
cp "$repo"/idydb/tests/fixtures/portable-v1*.db "$payload/fixtures/"
cp "$repo/LICENSE" "$payload/IdyDB-LICENSE.txt"
cp /build/windows-deps/share/openssl/copyright "$payload/OpenSSL-copyright.txt"
for package in gcc-mingw-w64-x86-64-posix-runtime mingw-w64-common; do
    cp "/usr/share/doc/$package/copyright" "$payload/$package-copyright.txt"
done

# Use a native Linux reader/writer to exercise both directions of interchange.
linux_build=/build/windows-interop-linux
cmake -S "$repo/idydb" -B "$linux_build" -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build "$linux_build" --target idydb_compatibility_tests --parallel 2
"$linux_build/idydb_compatibility_tests" --write "$payload/linux-fixtures"

for configuration in big-static tiny-static small-static big-shared; do
    sizing=3
    shared=OFF
    mmap=ON
    kind=Release
    case "$configuration" in
        big-static) kind=Debug ;;
        tiny-static) sizing=1 ;;
        small-static) sizing=2; mmap=OFF ;;
        big-shared) shared=ON; mmap=OFF ;;
    esac
    build="/build/windows-$configuration"
    prefix="$build/install"
    destination="$payload/$configuration"
    mkdir -p "$destination"
    cmake -S "$repo/idydb" -B "$build" \
        -DCMAKE_TOOLCHAIN_FILE="$repo/dev/mingw-x64.cmake" \
        -DCMAKE_BUILD_TYPE="$kind" -DBUILD_TESTING=ON \
        -DIDYDB_SIZING_MODE="$sizing" -DIDYDB_BUILD_SHARED="$shared" \
        -DIDYDB_ENABLE_MMAP="$mmap" \
        -DCMAKE_C_FLAGS='-Wall -Wextra -Werror' \
        -DCMAKE_CXX_FLAGS='-Wall -Wextra -Werror'
    cmake --build "$build" --parallel 2
    cmake --install "$build" --prefix "$prefix"
    consumer_openssl=''
    if [ "$shared" = ON ]; then consumer_openssl=-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE; fi
    cmake -S "$repo/idydb/tests/consumer" -B "$build/consumer" \
        -DCMAKE_TOOLCHAIN_FILE="$repo/dev/mingw-x64.cmake" \
        -DCMAKE_BUILD_TYPE="$kind" -DCMAKE_PREFIX_PATH="$prefix" \
        -Didydb_DIR="$prefix/lib/cmake/idydb" \
        -DCMAKE_C_FLAGS='-Wall -Wextra -Werror' \
        -DCMAKE_CXX_FLAGS='-Wall -Wextra -Werror' $consumer_openssl
    cmake --build "$build/consumer" --parallel 2
    cmake -S "$repo/idydb/tests/consumer" -B "$build/consumer-c-only" \
        -DCMAKE_TOOLCHAIN_FILE="$repo/dev/mingw-x64.cmake" \
        -DIDYDB_CONSUMER_C_ONLY=ON \
        -DCMAKE_BUILD_TYPE="$kind" -DCMAKE_PREFIX_PATH="$prefix" \
        -Didydb_DIR="$prefix/lib/cmake/idydb" \
        -DCMAKE_C_FLAGS='-Wall -Wextra -Werror' $consumer_openssl
    cmake --build "$build/consumer-c-only" --parallel 2
    for suite in core query persistence c_api failure directory platform compatibility; do
        if [ "$suite" = c_api ]; then file=idydb_c_api_test.exe
        else file="idydb_${suite}_tests.exe"; fi
        cp "$build/$file" "$destination/"
    done
    cp "$build/consumer-c-only/consumer_c.exe" "$build/consumer/consumer_cpp.exe" "$destination/"
    for library in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
        runtime=$("$compiler" -print-file-name="$library")
        test "$runtime" != "$library"
        test -f "$runtime"
        cp "$runtime" "$destination/"
    done
    if [ "$shared" = ON ]; then cp "$build/libidydb.dll" "$destination/"; fi
    # Archive only in the container: no compiler, SDK, or development libraries
    # are copied to the Windows runtime test directory.
    cpack --config "$build/CPackConfig.cmake" -B "$build/package" \
        -D "CPACK_PACKAGE_FILE_NAME=idydb-mingw-x64-$configuration"
    x86_64-w64-mingw32-nm -g --defined-only "$build/libidydb.a" > "$build/production-symbols.txt" 2>/dev/null || {
        if [ "$shared" != ON ]; then exit 1; fi
        x86_64-w64-mingw32-nm -g --defined-only "$build/libidydb.dll" > "$build/production-symbols.txt"
    }
    if awk '$NF ~ /idydb_test_/ { found = 1 } END { exit !found }' "$build/production-symbols.txt"; then
        echo "Fault hook exposed by production artifact: $configuration" >&2
        exit 1
    fi
done
cat > "$payload/manifest.json" <<EOF
{"schema":1,"run":"$run_id","configurations":["big-static","tiny-static","small-static","big-shared"]}
EOF
cp "$payload/manifest.json" /build/windows-runtime/latest.json
printf '\nWindows runtime payload ready: %s\n' "$payload"
