#!/bin/sh

# Create a delegated system service without running benchmark commands as root.
set -eu

if [ "$#" -lt 3 ] || [ "$1" != "--cpus" ]; then
    printf 'Usage: %s --cpus CPU_LIST CONFIG.toml [te-stsuite options...]\n' "$0" >&2
    exit 2
fi

if [ "$(id -u)" -eq 0 ]; then
    printf 'Run this script as your normal user; it invokes sudo for systemd.\n' >&2
    exit 2
fi

cpu_list=$2
shift 2
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
runner="$script_dir/build/te-stsuite"

# Build before elevating so the compiler and build files stay user-owned.
"$script_dir/build.sh"

# Place the unit under the root slice and delegate its cpuset subtree to the runner.
exec sudo systemd-run --system --wait --collect --pty \
    --uid="$(id -un)" \
    --slice=-.slice \
    -p "WorkingDirectory=$PWD" \
    -p "AllowedCPUs=$cpu_list" \
    -p CPUSetPartition=root \
    -p Delegate=yes \
    -p DelegateSubgroup=te-stsuite-controller \
    -E TE_STSUITE_DELEGATED_SUBGROUP=te-stsuite-controller \
    -E "PATH=$PATH" \
    -E "HOME=$HOME" \
    -E "DISPLAY=${DISPLAY-}" \
    -E "WAYLAND_DISPLAY=${WAYLAND_DISPLAY-}" \
    -E "XAUTHORITY=${XAUTHORITY-}" \
    -E "XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR-}" \
    -E "DBUS_SESSION_BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS-}" \
    -- "$runner" "$@"
