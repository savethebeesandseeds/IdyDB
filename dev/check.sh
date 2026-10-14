#!/bin/sh
# Run the complete regression matrix inside the managed Linux container.
set -eu
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export ASAN_OPTIONS=detect_leaks=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

for configuration in big tiny small shared sanitized; do
    build_dir="/build/check/$configuration"
    sizing_mode=3
    shared=OFF
    mmap=ON
    compile_flags='-Wall -Wextra -Werror'
    link_flags=''
    case "$configuration" in
        tiny) sizing_mode=1 ;;
        small) sizing_mode=2 ;;
        shared) shared=ON; mmap=OFF ;;
        sanitized)
            compile_flags="$compile_flags -fsanitize=address,undefined -fno-omit-frame-pointer"
            link_flags='-fsanitize=address,undefined'
            ;;
    esac
    printf '\nChecking %s configuration\n' "$configuration"
    cmake -S "$repo_root/idydb" -B "$build_dir" \
        -DBUILD_TESTING=ON \
        -DIDYDB_SIZING_MODE="$sizing_mode" \
        -DIDYDB_BUILD_SHARED="$shared" \
        -DIDYDB_ENABLE_MMAP="$mmap" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_FLAGS="$compile_flags" \
        -DCMAKE_CXX_FLAGS="$compile_flags" \
        -DCMAKE_EXE_LINKER_FLAGS="$link_flags" \
        -DCMAKE_SHARED_LINKER_FLAGS="$link_flags" \
        -DCMAKE_MODULE_LINKER_FLAGS="$link_flags"
    cmake --build "$build_dir" --parallel 2
    ctest --test-dir "$build_dir" --output-on-failure --parallel 2 --stop-on-failure

    library="$build_dir/libidydb.a"
    if [ "$shared" = ON ]; then library="$build_dir/libidydb.so"; fi
    nm -g --defined-only "$library" > "$build_dir/production-symbols.txt"
    if awk '$NF == "idydb_test_set_hook" { found = 1 } END { exit !found }' \
        "$build_dir/production-symbols.txt"; then
        echo "Test fault hook exposed by production library: $library" >&2
        exit 1
    fi
done

sh "$repo_root/dev/test-addon.sh"
