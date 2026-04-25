#!/bin/bash
set -e

sudo apt update

sudo apt install -y \
    build-essential \
    bc \
    bison \
    binutils-dev \
    ccache \
    curl \
    dwarves \
    fakeroot \
    flex \
    git \
    libbabeltrace-dev \
    libaudit-dev \
    libcap-dev \
    libdw-dev \
    libelf-dev \
    libncurses-dev \
    libnuma-dev \
    libperl-dev \
    libslang2-dev \
    libssl-dev \
    libtraceevent-dev \
    libtracefs-dev \
    libunwind-dev \
    numactl \
    pkg-config \
    python3-dev \
    rsync \
    systemtap-sdt-dev \
    wget \
    kexec-tools

cp /boot/config-$(uname -r) .config

NUMA_NODES=$(numactl --hardware | grep -c '^node [0-9]* cpus:')
echo "Detected $NUMA_NODES NUMA node(s)"

scripts/config --enable PARAVIRT_XXL
scripts/config --set-val HYDRA_NUMA_NODE_COUNT "$NUMA_NODES"

make olddefconfig
