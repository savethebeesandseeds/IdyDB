#!/bin/sh
# Optional dependencies only. Run in the existing managed Linux container.
# IdyDB builds, test execution, host exports, and container lifecycle are kept
# in the separate development launcher, never in this setup script.
set -eu
export DEBIAN_FRONTEND=noninteractive

fail() { printf '%s\n' "$*" >&2; exit 1; }
[ "$(uname -s)" = Linux ] || fail 'MinGW setup must run inside the Linux development container.'
[ -r /etc/os-release ] || fail 'Missing Linux distribution identity.'
. /etc/os-release
[ "$ID" = debian ] && [ "$VERSION_ID" = 13 ] || fail 'MinGW setup requires Debian 13.'
[ "$(dpkg --print-architecture)" = amd64 ] || fail 'MinGW setup requires a Debian amd64 container.'
[ "$(id -u)" = 0 ] || fail 'Container root is required to install dependency packages.'
[ "$#" = 0 ] || fail 'This dependencies-only script accepts no project or lifecycle actions.'

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
package_lock=$script_directory/mingw-packages.lock
source_lock=$script_directory/openssl-windows.lock
[ -s "$package_lock" ] && [ -s "$source_lock" ] || fail 'Missing pinned MinGW dependency locks.'

dependency_root=/build/windows-deps
cache_root=/build/windows-cache
source_root=/build/windows-sources
mkdir -p "$cache_root/apt/archives/partial" "$source_root"
set --
while IFS= read -r package; do
    case "$package" in ''|'#'*) continue ;; esac
    case "$package" in *=*) ;; *) fail "Unpinned dependency in $package_lock" ;; esac
    set -- "$@" "$package"
done < "$package_lock"
[ "$#" -gt 0 ] || fail 'The MinGW package lock is empty.'
apt-get update
apt-get -o "Dir::Cache::archives=$cache_root/apt/archives" install -y --no-install-recommends --no-remove "$@"
for package do
    name=${package%%=*}
    expected=${package#*=}
    actual=$(dpkg-query -W -f='${Version}' "$name")
    [ "$actual" = "$expected" ] || fail "Installed $name version $actual differs from pinned $expected."
done

version=
url=
sha256=
while IFS='=' read -r key value; do
    case "$key" in
        ''|'#'*) continue ;;
        version) version=$value ;;
        url) url=$value ;;
        sha256) sha256=$value ;;
        *) fail "Unknown OpenSSL lock field: $key" ;;
    esac
done < "$source_lock"
case "$version" in ''|*[!0-9.]*) fail 'Invalid pinned OpenSSL version.' ;; esac
case "$sha256" in *[!0-9a-f]*|'') fail 'Invalid pinned OpenSSL SHA256.' ;; esac
[ "${#sha256}" = 64 ] || fail 'The OpenSSL SHA256 must contain 64 hexadecimal characters.'
[ "$url" = "https://github.com/openssl/openssl/releases/download/openssl-$version/openssl-$version.tar.gz" ] ||
    fail 'OpenSSL must come from its pinned official GitHub release.'

archive=$cache_root/openssl-$version.tar.gz
source_directory=$source_root/openssl-$version-mingw64
copy_attribution() {
    [ -s "$source_directory/LICENSE.txt" ] || fail 'Missing OpenSSL license attribution source.'
    mkdir -p "$dependency_root/share/openssl"
    cp -- "$source_directory/LICENSE.txt" "$dependency_root/share/openssl/copyright"
    if [ -f "$source_directory/NOTICE" ]; then
        cp -- "$source_directory/NOTICE" "$dependency_root/share/openssl/NOTICE"
    fi
}
if [ ! -f "$archive" ]; then
    printf 'Downloading pinned OpenSSL %s source into the Linux volume.\n' "$version"
    curl --fail --location --proto '=https' --tlsv1.2 --retry 3 --output "$archive.part" "$url"
    printf '%s  %s\n' "$sha256" "$archive.part" | sha256sum -c -
    mv -- "$archive.part" "$archive"
fi
printf '%s  %s\n' "$sha256" "$archive" | sha256sum -c -

compiler_version=$(x86_64-w64-mingw32-gcc -dumpfullversion -dumpversion)
compiler_package=$(dpkg-query -W -f='${Version}' gcc-mingw-w64-x86-64-posix)
x86_64-w64-mingw32-gcc -v 2>&1 | grep -q 'Thread model: posix' || fail 'The MinGW compiler must use its POSIX thread runtime.'
configuration="openssl=$version sha256=$sha256 compiler=$compiler_version compiler-package=$compiler_package target=mingw64 cross=x86_64-w64-mingw32- no-shared no-tests libdir=lib"
marker=$dependency_root/.idydb-openssl-mingw.lock
if [ -d "$dependency_root" ]; then
    if [ -f "$marker" ]; then
        [ "$(cat "$marker")" = "$configuration" ] || fail "Preserving mismatched dependency prefix: $dependency_root"
    elif [ -n "$(find "$dependency_root" -mindepth 1 -maxdepth 1 -print -quit)" ]; then
        fail "Preserving unmanaged nonempty dependency prefix: $dependency_root"
    fi
fi
mkdir -p "$dependency_root"
printf '%s\n' "$configuration" > "$marker"
if [ -f "$dependency_root/.idydb-openssl-complete" ] && [ -s "$dependency_root/lib/libcrypto.a" ] &&
   [ -s "$dependency_root/include/openssl/opensslv.h" ]; then
    copy_attribution
    printf 'Reusing OpenSSL %s MinGW static dependencies at %s.\n' "$version" "$dependency_root"
    exit 0
fi

source_marker=$source_directory/.idydb-source.sha256
if [ -d "$source_directory" ]; then
    [ -f "$source_marker" ] && [ "$(cat "$source_marker")" = "$sha256" ] ||
        fail "Preserving unmanaged or mismatched OpenSSL source directory: $source_directory"
fi
mkdir -p "$source_directory"
printf '%s\n' "$sha256" > "$source_marker"
# Re-extracting this verified archive also completes a previously interrupted
# extraction, without deleting any source or build directory.
tar -xzf "$archive" --strip-components=1 -C "$source_directory"
cd "$source_directory"
printf 'Configuring OpenSSL %s for Windows x64 with MinGW GCC %s.\n' "$version" "$compiler_version"
perl ./Configure mingw64 --cross-compile-prefix=x86_64-w64-mingw32- no-shared no-tests \
    --prefix="$dependency_root" --openssldir="$dependency_root/ssl" --libdir=lib
build_log=$cache_root/openssl-$version-mingw-build.log
printf 'Building the OpenSSL dependency; compiler output is saved to %s.\n' "$build_log"
if ! make -j2 > "$build_log" 2>&1; then
    tail -n 80 "$build_log" >&2
    fail "The OpenSSL dependency build failed; inspect $build_log."
fi
if ! make install_sw >> "$build_log" 2>&1; then
    tail -n 80 "$build_log" >&2
    fail "The OpenSSL dependency install failed; inspect $build_log."
fi
copy_attribution
[ -s "$dependency_root/lib/libcrypto.a" ] && [ -s "$dependency_root/include/openssl/opensslv.h" ] ||
    fail 'The cross-compiled OpenSSL dependency installation is incomplete.'
printf '%s\n' "$configuration" > "$dependency_root/.idydb-openssl-complete"
printf 'Installed OpenSSL %s MinGW static dependencies at %s.\n' "$version" "$dependency_root"
