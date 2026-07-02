#!/bin/bash
set -e

echo 32768 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
sudo modprobe vfio-pci
sudo ./dpdk/usertools/dpdk-devbind.py -b vfio-pci 0000:27:00.0
