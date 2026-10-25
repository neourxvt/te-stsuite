#!/bin/sh

# Let Meson select the system C compiler and compile the project.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir="$script_dir/build"

if [ -f "$build_dir/meson-private/coredata.dat" ]; then
    meson setup --reconfigure "$build_dir" "$script_dir"
else
    meson setup "$build_dir" "$script_dir"
fi

exec meson compile -C "$build_dir"
