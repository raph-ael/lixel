# Arbeitsumgebung

Rechner: Ubuntu 26.04, Projektordner `~/orca/projects/lixel`.
Telefon: Pixel 6 `oriole`, Bootloader `slider-17.0-15681480` (entsperrt),
erreichbar über USB-Netz als `172.16.42.1` (Rechner `172.16.42.2`).
Benutzer `user`, Passwort/PIN beim Einrichten selbst gesetzt (auch sudo). Zusätzlich Root-Login per
SSH-Schlüssel (`~/.ssh/id_ed25519`), nur für die Entwicklung – später entfernen.

## Fremdquellen wiederherstellen

| Ziel | Quelle | Stand |
|---|---|---|
| `src/pmbootstrap` | https://gitlab.postmarketos.org/postmarketOS/pmbootstrap.git | e0dfe2cd (3.12.1; apt-Version ist zu alt) |
| `src/pmaports` | https://gitlab.postmarketos.org/postmarketOS/pmaports.git | 0de65636 |
| `src/pixel6-nura` | https://github.com/TheNoswad/pixel6-mainline-nura.git | a74e18de |
| `src/pmaports-nura` | git-Worktree von pmaports auf 688bea9d, darauf `src/pixel6-nura/scripts/apply.sh src/pmaports-nura`, dann `kernel/patches/pmaports-nura-version-bumps.diff` | |
| `ref/gs` | `git clone --depth 1 -b android-gs-raviole-5.10-android14-qpr3 https://android.googlesource.com/kernel/gs` | b4d74ef4 |
| `kernel/linux` | Tarball `https://github.com/m8l8th814n-eng/gs101-mainline/archive/850061c79264d018317e9f849701f165e138e20c.tar.gz`, darauf die 38 Patches aus `src/pmaports-nura/device/testing/linux-postmarketos-gs101/`, darauf `kernel/patches/0001-*.patch` | |
| `kernel/v4l2loopback-0.15.4` | https://github.com/v4l2loopback/v4l2loopback/archive/v0.15.4.tar.gz + `v4l2loopback-compressed-sizeimage.diff` aus dem Kernel-Paket | |

`kernel/extra.config` ist der `_extra_config`-Block aus dem Kernel-APKBUILD,
`kernel/local.config` schaltet `LOCALVERSION_AUTO` ab (Release heißt `7.2.0-rc3+`).

Pakete auf dem Rechner: `clang lld llvm bison flex libssl-dev libelf-dev bc lz4
zstd device-tree-compiler mkbootimg qemu-user-binfmt`, dazu `adb`/`fastboot`.

pmbootstrap (nur für Userspace-Pakete und Komplett-Images nötig):
`python3 src/pmbootstrap/pmbootstrap.py -c pmb-nura.cfg ...`, Arbeitsverzeichnis
`~/.local/var/pmbootstrap-nura`. Achtung: `build-debian.sh` registriert das
Host-binfmt für aarch64 neu; danach schlagen pmbootstrap-Crossbuilds fehl, bis
`echo -1 | sudo tee /proc/sys/fs/binfmt_misc/qemu-aarch64` ausgeführt wurde.

## Kernel bauen und aufspielen

```
kernel/build.sh            # ~5 min inkrementell, Ergebnis in kernel/out
kernel/deploy.sh reboot    # kopiert Module/Kernel, erzeugt boot.img auf dem Telefon,
                           # schreibt boot + vendor_boot in BEIDE Slots, startet neu
```

Leistungsmessmodul: `make -C kernel/linux ARCH=arm64 LLVM=1 M=$PWD/kernel/odpm modules`,
dann `pixel6-odpm.ko` nach `/root/` kopieren und `insmod`.

## Boot-Ablauf und Gerätezustand

- Stock-Bootloader lädt `boot_X` + `vendor_boot_X` (Header v4). `dtbo_X` muss
  leer sein (sonst „failed to merge dtb and dtbo"), `vbmeta_X` mit abgeschalteter
  Prüfung. Beide Slots (a und b) enthalten inzwischen dasselbe Linux.
- Rootfs: GPT-Image in `userdata` (pmOS_boot ext2, pmOS_root ext4).
- Unangetastet: Bootloader, modem, efs, persist. Nie schreiben, nie einen älteren
  Bootloader flashen, Bootloader nicht wieder sperren.
- Zurück zu Android: Factory-Image + `flash-all.sh`.

## Notfälle

- **„GrapheneOS Recovery – Cannot load Android system":** nichts auswählen,
  vor allem keinen Factory Reset (löscht das Linux). Ursache war der A/B-
  Startzähler. `adb reboot bootloader`, `fastboot set_active b`, `fastboot reboot`.
- **Kernel startet nicht:** Vol-runter + Power → Fastboot, dann die zuletzt
  funktionierenden `boot.img`/`vendor_boot.img` flashen (lokal in
  `kernel/boot-good/`, nicht im Repo; ersatzweise per pmbootstrap neu erzeugen).
- **Hängt im Tiefschlaf:** Power ca. 30 s halten.
- `fastboot oem dmesg` liefert das Bootloader-Protokoll.
