#!/bin/sh

# Set up the default musl toolchain and compile the Meson project.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir="$script_dir/build"
native_file="$script_dir/meson/musl.ini"

if [ -f "$build_dir/meson-private/coredata.dat" ]; then
    meson setup "$build_dir" "$script_dir" --reconfigure --native-file "$native_file"
else
    meson setup "$build_dir" "$script_dir" --native-file "$native_file"
fi

exec meson compile -C "$build_dir"
