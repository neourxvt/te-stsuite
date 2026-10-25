#!/bin/sh

# Rebuild the runner and execute it with the supplied TOML configuration.
set -eu

if [ "$#" -lt 1 ]; then
    printf 'Usage: %s CONFIG.toml [cvtebench options...]\n' "$0" >&2
    exit 2
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
"$script_dir/build.sh"
exec "$script_dir/build/cvtebench" "$@"
