#!/bin/sh

# Set up the default musl toolchain and compile the Meson project.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir="$script_dir/build"
native_file="$script_dir/meson/musl.ini"

# Alpine provides a musl-targeting cc/gcc instead of the musl-gcc wrapper.
compiler=musl-gcc
if ! command -v "$compiler" >/dev/null 2>&1; then
    if command -v cc >/dev/null 2>&1; then
        compiler=cc
    elif command -v gcc >/dev/null 2>&1; then
        compiler=gcc
    else
        printf 'build.sh: no C compiler found\n' >&2
        exit 1
    fi
    compiler_macros=$(printf '#include <features.h>\n' |
        "$compiler" -dM -E -x c - 2>/dev/null) || {
        printf 'build.sh: cannot inspect the selected C compiler\n' >&2
        exit 1
    }
    case "$compiler_macros" in
        *__GLIBC__*)
            printf 'build.sh: musl-gcc is missing and %s targets glibc\n' \
                "$compiler" >&2
            exit 1
            ;;
    esac
fi

# Keep the normal musl toolchain file, while allowing Alpine's native musl cc.
meson_setup() {
    if [ "$compiler" = musl-gcc ]; then
        meson setup "$@" --native-file "$native_file"
    else
        CC="$compiler" meson setup "$@"
    fi
}

if [ -f "$build_dir/meson-private/coredata.dat" ]; then
    meson_setup --reconfigure "$build_dir" "$script_dir"
else
    meson_setup "$build_dir" "$script_dir"
fi

exec meson compile -C "$build_dir"
