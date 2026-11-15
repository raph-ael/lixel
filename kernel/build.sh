#!/bin/bash
# Host build of the gs101 kernel (mirrors the fork's APKBUILD build()/package()).
# Output: out/boot/{vmlinuz,gs101-oriole.dtb}, out/usr/lib/modules/<rel>
set -euo pipefail
K=$(cd "$(dirname "$0")" && pwd)
B=$K/linux; O=$K/out; V=$K/v4l2loopback-0.15.4
PK=/home/raphael/orca/projects/lixel/src/pmaports-nura/device/testing/linux-postmarketos-gs101
M="make -C $B ARCH=arm64 LLVM=1 -j$(nproc)"
IOMMU="CONFIG_SAMSUNG_IOMMU=m CONFIG_SAMSUNG_IOMMU_GROUP=m"
AUDIO="CONFIG_TRUSTY=m CONFIG_TRUSTY_IRQ=y CONFIG_TRUSTY_LOG=m CONFIG_TRUSTY_VIRTIO=m CONFIG_TRUSTY_VIRTIO_IPC=m CONFIG_GSA=m CONFIG_WC_MBOX=m CONFIG_AOC_DRIVER=m CONFIG_AOC_CHAR_DRIVER=m CONFIG_AOC_CHAN_DRIVER=m CONFIG_AOC_CONTROL_DRIVER=m"
GPS="CONFIG_BCM_GPS_SPI_DRIVER=m CONFIG_BCM_GPS_PPS_DRIVER=m"
MODEM="CONFIG_EXYNOS_MODEM_IF=m CONFIG_SHM_IPC=m CONFIG_BOOT_DEVICE_SPI=m"
if [ ! -f $B/.config ] || [ "${1:-}" = config ]; then
    cp $PK/config-postmarketos-gs101.aarch64 $B/.config
    cat $K/extra.config >> $B/.config
    [ -f $K/local.config ] && cat $K/local.config >> $B/.config
    $M olddefconfig
fi
$M KBUILD_BUILD_VERSION=99-lixel Image.lz4 modules dtbs
ext() { # dir, extra args...
    local d=$1; shift
    $M M="$d" "$@" modules
    $M M="$d" "$@" INSTALL_MOD_PATH=$O/usr INSTALL_MOD_STRIP=1 modules_install
}
rm -rf $O; mkdir -p $O/boot
$M INSTALL_MOD_PATH=$O/usr INSTALL_MOD_STRIP=1 modules_install >/dev/null
ext $B/bcmdhd BCMDHD_ROOT=$B/bcmdhd
ext $B/samsung-iommu $IOMMU
ext $B/modem $MODEM
ext $B/gps $GPS
ext $B/trusty $AUDIO
ext $B/gsa $AUDIO
ext $B/aoc $AUDIO KBUILD_EXTRA_SYMBOLS="$B/trusty/Module.symvers $B/gsa/Module.symvers"
ext $B/cs35l41dna $AUDIO
ext $V
rm -f $O/usr/lib/modules/*/build $O/usr/lib/modules/*/source
cp $B/arch/arm64/boot/Image.lz4 $O/boot/vmlinuz
cp $B/arch/arm64/boot/dts/exynos/google/gs101-oriole.dtb $O/boot/
cat $B/include/config/kernel.release; echo BUILD_OK
