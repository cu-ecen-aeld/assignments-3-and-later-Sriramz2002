#!/bin/bash
# Script to install and build the Linux kernel and root filesystem.
# Author: Sriramkumar Jayaraman 

set -e
set -u


# Define Output Directory
OUTDIR="${HOME}/lk"
KERNEL_REPO=https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git

KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-

# Required Dependencies
PACKAGES=(
    flex
    bison
    build-essential
    libssl-dev
    bc
    u-boot-tools
    qemu
    cpio
    device-tree-compiler
)

# Install Missing Dependencies
sudo apt update && sudo apt install -y "${PACKAGES[@]}"

# Allow Custom Output Directory
if [ $# -ge 1 ]; then
    OUTDIR=$1
    echo "Using provided output directory: ${OUTDIR}"
else
    echo "Using default directory: ${OUTDIR}"
fi

mkdir -p "$OUTDIR" || { echo "Failed to create ${OUTDIR}"; exit 1; }
cd "$OUTDIR"

### Clone Linux Kernel ###
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    echo "Cloning Linux kernel version ${KERNEL_VERSION}"
    git clone ${KERNEL_REPO} --depth 1 --branch ${KERNEL_VERSION} linux-stable
fi

### Build Linux Kernel ###
if [ ! -e "${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image" ]; then
    cd linux-stable
    echo "Building Kernel................................................"
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} mrproper
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
    
    #-j is for parallel processing : taken from chatgpt
    make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} all
    make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} modules 
    make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} dtbs
    cp arch/${ARCH}/boot/Image ${OUTDIR}/
    cp -r arch/${ARCH}/boot/dts/ ${OUTDIR}/
fi

echo "Kernel Image and Device Tree copied successfully"







### Create Root Filesystem ###
echo "Setting up root filesystem........................"
cd "$OUTDIR"
sudo rm -rf ${OUTDIR}/rootfs
mkdir -p rootfs/{bin,sbin,etc,proc,sys,dev,usr,lib,lib64,var,tmp,home}
mkdir -p rootfs/usr/{bin,lib,sbin}
mkdir -p rootfs/var/log

### Clone & Build BusyBox ###
cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]; then
    git clone git://busybox.net/busybox.git
    cd busybox
    git checkout ${BUSYBOX_VERSION}
else
    cd busybox
fi

make distclean
make defconfig
make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE}
make CONFIG_PREFIX=${OUTDIR}/rootfs ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} install

# Ensure busybox binary exists
if [ ! -f "${OUTDIR}/rootfs/bin/busybox" ]; then
    echo "Error: BusyBox installation failed!"
    exit 1
fi

echo "BusyBox installed successfully."








### Set Permissions: taken from stackoverflow ###
sudo chmod u+s ${OUTDIR}/rootfs/bin/busybox

### Copy Library Dependencies ###
SYSROOT=$(${CROSS_COMPILE}gcc --print-sysroot)

# Ensure both `lib` and `lib64` are copied correctly
sudo cp -a ${SYSROOT}/lib/ld-linux-aarch64.so.1 ${OUTDIR}/rootfs/lib/ || echo "Warning: ld-linux-aarch64.so.1 missing!"
sudo cp -a ${SYSROOT}/lib64/* ${OUTDIR}/rootfs/lib64/ || echo "Warning: lib64 missing!"

# Additional critical libraries for `aarch64`
sudo cp -a ${SYSROOT}/lib64/libc.so.6 ${OUTDIR}/rootfs/lib64/ || echo "Warning: libc.so.6 missing!"
sudo cp -a ${SYSROOT}/lib64/libm.so.6 ${OUTDIR}/rootfs/lib64/ || echo "Warning: libm.so.6 missing!"
sudo cp -a ${SYSROOT}/lib64/libresolv.so.2 ${OUTDIR}/rootfs/lib64/ || echo "Warning: libresolv.so.2 missing!"
sudo cp -a ${SYSROOT}/lib64/libdl.so.2 ${OUTDIR}/rootfs/lib64/ || echo "Warning: libdl.so.2 missing!"




### Create Device Nodes ###
sudo mknod -m 666 ${OUTDIR}/rootfs/dev/null c 1 3
sudo mknod -m 600 ${OUTDIR}/rootfs/dev/console c 5 1

### copy and make finder-test.sh ###
cd ${FINDER_APP_DIR}
make clean
make CROSS_COMPILE=${CROSS_COMPILE}
cp writer ${OUTDIR}/rootfs/home/





##Copy Finder App Scripts #
cp ${FINDER_APP_DIR}/finder.sh ${OUTDIR}/rootfs/home/
cp ${FINDER_APP_DIR}/finder-test.sh ${OUTDIR}/rootfs/home/
cp ${FINDER_APP_DIR}/writer ${OUTDIR}/rootfs/home/
cp ${FINDER_APP_DIR}/autorun-qemu.sh ${OUTDIR}/rootfs/home/
cp -r ${FINDER_APP_DIR}/conf/ ${OUTDIR}/rootfs/home/

### Fix Finder Test Config Path ###
sed -i 's|\.\./conf/assignment.txt|conf/assignment.txt|g' ${OUTDIR}/rootfs/home/finder-test.sh

### Set Correct Ownership ###
sudo chown -R root:root ${OUTDIR}/rootfs/

### Create Initramfs ###
cd ${OUTDIR}/rootfs
find . | cpio -H newc -ov --owner=root:root > ${OUTDIR}/initramfs.cpio
gzip -f ${OUTDIR}/initramfs.cpio

### Final Message ###
echo " Kernel, root filesystem, and device tree successfully built and ready for QEMU boot!"

