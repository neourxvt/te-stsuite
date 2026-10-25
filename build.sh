#!/bin/sh

# Configure a static musl build, recreating Meson's cache if its compiler changed.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$script_dir"
compiler=${CC:-musl-gcc}

if ! command -v "$compiler" >/dev/null 2>&1; then
    printf 'cvtebench: musl compiler not found: %s\n' "$compiler" >&2
    printf 'Install musl-gcc or set CC to a musl-targeting compiler.\n' >&2
    exit 1
fi

if [ -f build/meson-private/coredata.dat ]; then
    configured_compiler=$(meson introspect --compilers build |
        sed -n 's/.*"exelist": \["\([^"]*\)"\].*/\1/p')
    if [ "$configured_compiler" = "$compiler" ]; then
        CC="$compiler" meson setup build --reconfigure
    else
        CC="$compiler" meson setup build --wipe
    fi
else
    CC="$compiler" meson setup build
fi
meson compile -C build
