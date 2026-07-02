#!/bin/bash
set -e

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DPDK_PREFIX="${ROOT_DIR}/dpdk/build-install"

# DPDK on Debian/Ubuntu commonly installs to lib/x86_64-linux-gnu
if [ -d "${DPDK_PREFIX}/lib/x86_64-linux-gnu" ]; then
  DPDK_LIB_DIR="${DPDK_PREFIX}/lib/x86_64-linux-gnu"
else
  DPDK_LIB_DIR="${DPDK_PREFIX}/lib"
fi

APP_BIN="${ROOT_DIR}/multilane-server/build/multilane-server"

# SCHED_FIFO real-time priority (1-99; higher = more important than normal processes).
SCHED_RT_PRIORITY="${SCHED_RT_PRIORITY:-99}"

EAL_ARGS=( -l 32-47 -n 8 )
MULTILANE_SERVER_ARGS=( -p 0x1 -L -T -A synthetic )

sudo chrt -f "${SCHED_RT_PRIORITY}" \
  env LD_LIBRARY_PATH="${DPDK_LIB_DIR}:${LD_LIBRARY_PATH:-}" \
  "${APP_BIN}" "${EAL_ARGS[@]}" -- "${MULTILANE_SERVER_ARGS[@]}"
