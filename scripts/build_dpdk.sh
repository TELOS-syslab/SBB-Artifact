#!/bin/bash
set -e

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DPDK_SRC="${ROOT_DIR}/dpdk"
BUILD_DIR="${DPDK_SRC}/build"
PREFIX_DIR="${DPDK_SRC}/build-install"

rm -rf "${BUILD_DIR}"
rm -rf "${PREFIX_DIR}"

meson setup "${BUILD_DIR}" "${DPDK_SRC}" \
  --prefix="${PREFIX_DIR}" \
  -Ddefault_library=shared

ninja -C "${BUILD_DIR}" -j "$(nproc)"
ninja -C "${BUILD_DIR}" install
