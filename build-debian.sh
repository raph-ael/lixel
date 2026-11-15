#!/bin/bash
# Builds a Debian (trixie, arm64) rootfs image for the Pixel 6 (oriole) that
# boots with the postmarketOS gs101 mainline kernel + initramfs.
# The image keeps the pmOS partition layout/UUIDs so the pmOS boot.img
# (already flashed to boot/vendor_boot) finds and mounts it unchanged.
# Run as root: sudo ./build-debian.sh
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
PMWORK=${PMWORK:-/home/raphael/.local/var/pmbootstrap-lixel}
PMIMG=$PMWORK/chroot_native/home/pmos/rootfs/google-oriole.img
PMROOT=$PMWORK/chroot_rootfs_google-oriole
OUT=$HERE/out
IMG=$OUT/debian-oriole.img
MNT=$OUT/mnt
SUITE=trixie
IMG_SIZE=8G
USERNAME=user
PASSWORD=${PASSWORD:-changeme}   # set PASSWORD=... when running
SSHKEY=/home/raphael/.ssh/id_ed25519.pub
ROOT_UUID=75a2f476-75c3-4d7d-8b25-def4057d0b4b
BOOT_UUID=803b6823-7d6d-4623-b251-61a4d662088d

cleanup() {
    set +e
    for m in dev/pts dev proc sys; do mountpoint -q "$MNT/$m" && umount -l "$MNT/$m"; done
    mountpoint -q "$MNT" && umount "$MNT"
    [ -n "${LOOP:-}" ] && losetup -d "$LOOP"
}
trap cleanup EXIT

mkdir -p "$OUT" "$MNT"
systemctl restart systemd-binfmt
test -e /proc/sys/fs/binfmt_misc/qemu-aarch64

echo "== image"
rm -f "$IMG"
cp --sparse=always "$PMIMG" "$IMG"
truncate -s $IMG_SIZE "$IMG"
LOOP=$(losetup -f --show -b 4096 -P "$IMG")
START=$(sgdisk -i 2 "$LOOP" | awk '/First sector/ {print $3}')
sgdisk -e "$LOOP" >/dev/null
sgdisk -d 2 -n 2:"$START":0 -c 2:primary "$LOOP" >/dev/null
partprobe "$LOOP"; sleep 1
mkfs.ext4 -q -F -L pmOS_root -U $ROOT_UUID "${LOOP}p2"
mount "${LOOP}p2" "$MNT"

echo "== debootstrap"
debootstrap --arch=arm64 --variant=minbase \
    --include=systemd-sysv,udev,dbus,sudo,locales,ca-certificates,kmod,iproute2 \
    $SUITE "$MNT" http://deb.debian.org/debian

echo "== kernel modules + firmware from pmOS"
mkdir -p "$MNT/usr/lib/modules" "$MNT/usr/lib/firmware"
for d in "$PMROOT/lib/modules" "$PMROOT/usr/lib/modules"; do
    [ -d "$d" ] && cp -a "$d/." "$MNT/usr/lib/modules/" && break
done
for d in "$PMROOT/lib/firmware" "$PMROOT/usr/lib/firmware"; do
    [ -d "$d" ] && cp -a "$d/." "$MNT/usr/lib/firmware/" && break
done

echo "== configure"
cat > "$MNT/etc/apt/sources.list" <<EOF
deb http://deb.debian.org/debian $SUITE main contrib non-free non-free-firmware
deb http://deb.debian.org/debian $SUITE-updates main contrib non-free non-free-firmware
deb http://security.debian.org/debian-security $SUITE-security main contrib non-free non-free-firmware
EOF
echo lixel > "$MNT/etc/hostname"
printf '127.0.0.1 localhost\n127.0.1.1 lixel\n' > "$MNT/etc/hosts"
cat > "$MNT/etc/fstab" <<EOF
UUID=$ROOT_UUID / ext4 defaults 0 1
UUID=$BOOT_UUID /boot ext2 nodev,nosuid,noexec,nofail 0 0
EOF
cp /etc/resolv.conf "$MNT/etc/resolv.conf.build"
rm -f "$MNT/etc/resolv.conf"; cp /etc/resolv.conf "$MNT/etc/resolv.conf"

for m in proc sys dev dev/pts; do mount --bind "/$m" "$MNT/$m"; done
printf '#!/bin/sh\nexit 101\n' > "$MNT/usr/sbin/policy-rc.d"; chmod +x "$MNT/usr/sbin/policy-rc.d"

chroot "$MNT" /bin/bash -euxc "
export DEBIAN_FRONTEND=noninteractive LC_ALL=C
apt-get update -q
apt-get install -y -q --no-install-recommends \
    openssh-server network-manager systemd-resolved systemd-timesyncd \
    e2fsprogs less nano vim-tiny htop usbutils pciutils iputils-ping curl wget \
    bash-completion console-setup-linux kbd \
    phosh phoc squeekboard phosh-mobile-settings gnome-console \
    gnome-session-bin gnome-settings-daemon gsettings-desktop-schemas \
    xdg-desktop-portal-gtk adwaita-icon-theme fonts-cantarell fonts-noto-core \
    libgl1-mesa-dri mesa-utils dbus-user-session polkitd pipewire wireplumber \
    feedbackd iio-sensor-proxy upower
sed -i 's/^# *\(en_US.UTF-8\|de_DE.UTF-8\)/\1/' /etc/locale.gen && locale-gen
update-locale LANG=de_DE.UTF-8
useradd -m -u 1000 -s /bin/bash -G sudo,video,audio,input,render,netdev,plugdev $USERNAME
echo '$USERNAME:$PASSWORD' | chpasswd
systemctl enable ssh systemd-networkd NetworkManager systemd-resolved phosh || true
apt-get clean
"
rm -f "$MNT/usr/sbin/policy-rc.d"

install -d -m 700 -o 1000 -g 1000 "$MNT/home/$USERNAME/.ssh"
install -m 600 -o 1000 -g 1000 "$SSHKEY" "$MNT/home/$USERNAME/.ssh/authorized_keys"

# USB gadget network (gadget itself is set up by the pmOS initramfs and persists)
cat > "$MNT/etc/systemd/network/10-usb0.network" <<EOF
[Match]
Name=usb0

[Network]
Address=172.16.42.1/24
DHCPServer=yes
Gateway=172.16.42.2
DNS=1.1.1.1
ConfigureWithoutCarrier=yes

[DHCPServer]
PoolOffset=2
PoolSize=1
EmitDNS=no
EmitRouter=no
EOF
mkdir -p "$MNT/etc/NetworkManager/conf.d"
printf '[keyfile]\nunmanaged-devices=interface-name:usb0\n' > "$MNT/etc/NetworkManager/conf.d/usb0.conf"
ln -sf /run/systemd/resolve/stub-resolv.conf "$MNT/etc/resolv.conf"
rm -f "$MNT/etc/resolv.conf.build"
depmod -b "$MNT/usr" "$(ls "$MNT/usr/lib/modules" | head -1)" 2>/dev/null || true

echo "== finish"
df -h "$MNT" | tail -1
cleanup; trap - EXIT
chown raphael: "$IMG"
ls -la "$IMG"
echo BUILD_OK
