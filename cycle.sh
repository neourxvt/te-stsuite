#!/bin/sh

# Rebuild the runner and select a launcher appropriate for the current manager.
set -eu

if [ "$#" -lt 1 ]; then
    printf 'Usage: %s CONFIG.toml [cvtebench options...]\n' "$0" >&2
    exit 2
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
runner="$script_dir/build/cvtebench"

# Prefer systemd's native delegation when a user manager is available.
container_manager=
if command -v systemd-detect-virt >/dev/null 2>&1; then
    container_manager=$(systemd-detect-virt --container 2>/dev/null || true)
    case "$container_manager" in
        ""|none) container_manager= ;;
    esac
fi
if [ -z "$container_manager" ] && [ -f /.dockerenv ]; then
    container_manager=docker
elif [ -z "$container_manager" ] && [ -f /run/.containerenv ]; then
    container_manager=podman
elif [ -z "$container_manager" ] && [ -r /run/systemd/container ]; then
    IFS= read -r container_manager < /run/systemd/container || true
fi

init_name=unknown
if [ -r /proc/1/comm ]; then
    IFS= read -r init_name < /proc/1/comm || true
fi
systemd_user_manager=false
if command -v systemd-run >/dev/null 2>&1 &&
   command -v systemctl >/dev/null 2>&1 &&
   systemctl --user show-environment >/dev/null 2>&1; then
    systemd_user_manager=true
fi
if [ -n "$container_manager" ]; then
    printf 'Detected container environment: %s\n' "$container_manager"
elif [ "$init_name" = systemd ]; then
    printf 'Detected system manager: systemd\n'
elif [ "$systemd_user_manager" = true ]; then
    printf 'Detected systemd user manager (PID 1: %s)\n' "$init_name"
else
    printf 'Detected non-systemd init: %s\n' "$init_name"
fi

launcher=direct
if [ "$systemd_user_manager" = true ]; then
    launcher=systemd-user-scope
    printf 'Launch mode: delegated transient systemd user scope\n'
elif [ -n "$container_manager" ]; then
    printf 'Launch mode: use the cgroup delegation supplied by the container manager\n'
else
    printf 'Launch mode: use the current cgroup delegation\n'
fi

"$script_dir/build.sh"
if [ "$launcher" = systemd-user-scope ]; then
    exec systemd-run --user --scope --property=Delegate=yes -- "$runner" "$@"
fi
exec "$runner" "$@"
