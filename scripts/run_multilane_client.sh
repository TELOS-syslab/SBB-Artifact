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

APP_BIN="${ROOT_DIR}/multilane-client/build/multilane-client"

EAL_ARGS=( -l 0-192 -n 8 )
MULTILANE_CLIENT_ARGS=( \
  -c 30000000 \
  -l 2000000 \
  -p 0x2 \
  -t 1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31,33,35,37,39,41,43,45,47,49,51,53,55,57,59,61,63,65,67,69,71,73,75,77,79,81,83,85,87,89,91,93,95,97,99,101 \
  -r 103,105,107,109,111,113,115,117,119,121,123,125,127,129,131,133,135,137,139,141,143,145,147,149,151,153,155,157,159,161,163,165,167,169,171,173,175,177,179,181,183,185,187,189,191 \
  -A synthetic \
  -d fixed_1 \
)

sudo LD_LIBRARY_PATH="${DPDK_LIB_DIR}:${LD_LIBRARY_PATH:-}" \
  "${APP_BIN}" "${EAL_ARGS[@]}" -- "${MULTILANE_CLIENT_ARGS[@]}"

