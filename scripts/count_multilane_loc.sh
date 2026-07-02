#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

count_loc() {
    local target_dir="$1"

    find "${target_dir}" \
        -type d -name build -prune -o \
        -type d -name applications -prune -o \
        -type f \
        \( \
            -name "*.c" -o \
            -name "*.h" -o \
            -name "*.cc" -o \
            -name "*.cpp" -o \
            -name "*.cxx" -o \
            -name "*.hpp" -o \
            -name "*.hh" -o \
            -name "*.hxx" -o \
            -name "*.py" -o \
            -name "*.sh" -o \
            -name "meson.build" -o \
            -name "Makefile" \
        \) \
        -print0 | xargs -0 wc -l | tail -n 1 | awk '{print $1}'
}

server_dir="${REPO_ROOT}/multilane-server"
client_dir="${REPO_ROOT}/multilane-client"

server_loc="$(count_loc "${server_dir}")"
client_loc="$(count_loc "${client_dir}")"

printf 'multilane-server: %s lines\n' "${server_loc}"
printf 'multilane-client: %s lines\n' "${client_loc}"
