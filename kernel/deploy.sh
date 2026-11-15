#!/bin/bash
# Install the host-built kernel (out/) on the running phone and flash
# boot/vendor_boot of both slots from there. Known-good images: boot-good/.
# Recovery if it does not boot: hold Vol-down + Power -> fastboot, then
#   fastboot flash boot boot-good/boot.img; fastboot flash vendor_boot boot-good/vendor_boot.img
set -euo pipefail
K=$(cd "$(dirname "$0")" && pwd); O=$K/out
SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 -o BatchMode=yes root@172.16.42.1"
REL=$(ls $O/usr/lib/modules)
echo "release $REL"
tar -C $O -cf - usr/lib/modules boot | $SSH "
set -e
rm -rf /usr/lib/modules.new; mkdir -p /tmp/kdeploy; cd /tmp/kdeploy; rm -rf usr boot; tar -xf -
rm -rf /usr/lib/modules/$REL.old
[ -d /usr/lib/modules/$REL ] && mv /usr/lib/modules/$REL /usr/lib/modules/$REL.old
mv usr/lib/modules/$REL /usr/lib/modules/$REL
rm -rf /usr/lib/modules/$REL.old
cp boot/vmlinuz /boot/vmlinuz
cp boot/gs101-oriole.dtb /boot/dtbs/exynos/google/gs101-oriole.dtb
echo $REL > /usr/share/kernel/postmarketos-gs101/kernel.release
depmod $REL
mkinitfs >/tmp/kdeploy/mkinitfs.log 2>&1 || { tail -20 /tmp/kdeploy/mkinitfs.log; exit 1; }
ls -la /boot/boot.img /boot/vendor_boot.img
# Both slots carry Linux: the bootloader falls back to the other slot when
# the active one runs out of boot retries (nothing marks it successful).
for s in a b; do
dd if=/boot/boot.img of=/dev/disk/by-partlabel/boot_\$s bs=1M conv=fsync 2>&1 | tail -1
dd if=/boot/vendor_boot.img of=/dev/disk/by-partlabel/vendor_boot_\$s bs=1M conv=fsync 2>&1 | tail -1
done
sync
rm -rf /tmp/kdeploy/usr /tmp/kdeploy/boot
"
[ "${1:-}" = reboot ] && { $SSH "systemctl reboot" || true; echo rebooting; }
