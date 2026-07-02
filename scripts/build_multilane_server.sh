#!/bin/bash
set -e

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DPDK_PREFIX="${ROOT_DIR}/dpdk/build-install"
DPDK_PKGCFG_DIR="${DPDK_PREFIX}/lib/x86_64-linux-gnu/pkgconfig"
PKGCFG="${DPDK_PKGCFG_DIR}/libdpdk.pc"
APP_DIR="${ROOT_DIR}/multilane-server"
APP_BUILD_DIR="${APP_DIR}/build"

if [ ! -f "${PKGCFG}" ]; then
  echo "[INFO] DPDK not found in ${DPDK_PREFIX}, building DPDK first..."
  "${ROOT_DIR}/scripts/build_dpdk.sh"
fi

export PKG_CONFIG_PATH="${DPDK_PKGCFG_DIR}:${PKG_CONFIG_PATH}"

if [ ! -f "${APP_BUILD_DIR}/build.ninja" ]; then
  meson setup "${APP_BUILD_DIR}" "${APP_DIR}"
else
  meson setup --reconfigure "${APP_BUILD_DIR}" "${APP_DIR}"
fi

ninja -C "${APP_BUILD_DIR}" -j "$(nproc)"

echo "[DONE] Build complete: ${APP_BUILD_DIR}/multilane-server"
