#!/bin/bash
# Executes each step in order and stops if any command fails

set -e  # Exit immediately on error

echo "=== Building kernel ==="
if ! make -j"$(nproc)"; then
    echo "Failed: Kernel build"
    exit 1
fi

echo "=== Building modules ==="
if ! make modules -j"$(nproc)"; then
    echo "Failed: Module build"
    exit 1
fi

echo "=== Installing modules ==="
if ! sudo make modules_install; then
    echo "Failed: Module install"
    exit 1
fi

echo "=== Installing kernel ==="
if ! sudo make install; then
    echo "Failed: Kernel install"
    exit 1
fi

echo "=== Updating GRUB ==="
if ! sudo update-grub; then
    echo "Failed: GRUB update"
    exit 1
fi

echo "Success: All steps completed successfully. Please reboot."
