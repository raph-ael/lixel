# Protokoll

## 14.11.2026 – Linux aufs Gerät

1. Ausgangslage: Pixel 6 mit GrapheneOS (Android 17), Kiosk-Launcher. OEM-Entsperrung
   vom Besitzer aktiviert, Bootloader entsperrt (alle Daten gelöscht).
2. Offizielles postmarketOS (pmaports `device-google-oriole`, Kernel 7.3-rc6):
   bootet, aber nur Display/Touch/USB.
   - pmbootstrap aus apt (3.9.0) zu alt → Git-Version 3.12.1.
   - `fastboot boot` aus dem RAM tut nichts → flashen.
   - `flash_rootfs`: „snapshot update in progress" → `fastboot snapshot-update cancel`.
   - Bootfehler „failed to merge dtb and dtbo" → `fastboot erase dtbo`.
3. Eigenes Debian 13 (`build-debian.sh`) mit pmOS-Kernel/Initramfs: bootet, Phosh läuft.
4. Priorität geändert: erst Telefonfunktionen. Fork `pixel6-mainline-nura` gebaut
   und geflasht (Kernel 7.2-rc3, bcmdhd, cpif-Modem, Panfrost, AoC-Audio,
   gepatchter ModemManager).
   - Build brach mit `cc1: posix_spawnp` ab: Host-binfmt von `build-debian.sh` → abmelden.
   - pmbootstrap installierte still den offiziellen Kernel bzw. das offizielle Mesa,
     weil deren Versionen höher waren → `pkgver` hochgesetzt
     (`kernel/patches/pmaports-nura-version-bumps.diff`).
5. Ergebnis: WLAN-Scan, Mobilfunk + mobile Daten (sim.de/1&1, APN `internet`),
   nach Mesa-Fix flüssige Oberfläche bei 90 Hz.

## 15.11.2026 – Standby

1. Bestandsaufnahme: keine Power-Domains, `clk_ignore_unused`, nur C2; Fork-Doku
   sagt selbst, dass s2idle nichts spart und Tiefschlaf nicht aufwacht.
2. Kernel-Entwicklungszyklus auf dem Rechner eingerichtet (`kernel/build.sh`,
   `kernel/deploy.sh`), Googles Herstellerkernel als Referenz geklont.
3. Herstellerpfad analysiert (`exynos-pm.c`, `exynos-cpupm.c`, `cal-if/gs101`):
   Mainline setzt weder SLEEP-Hinweis noch Wake-Masken. Tiefschlaf- und
   SICD-Unterstützung in `exynos-pmu.c` ergänzt.
4. Zwischenfälle:
   - Lesen von `/dev/mem` bei 0x2039000 (APM-SRAM) setzt das Telefon zurück. Auch
     ein Wort-Scan im Kernel bricht ab; nur der Weg über die Plugin-Tabellen
     funktioniert (FLEXPMU_DBG liegt bei SRAM+0x26388).
   - Eigener Debug-Dump las ein nicht vorhandenes PMU-Register (0x2d80) → Oops, behoben.
   - „GrapheneOS Recovery"-Bildschirm: A/B-Startzähler abgelaufen, kein Kernelfehler.
     `fastboot set_active b`; danach Linux in beide Slots gespiegelt.
   - pstore/ramoops war nach einem Absturz leer – als Diagnosequelle bisher wertlos.
5. Tests: siehe Tabelle in STATUS.md. Der echte Tiefschlaf blieb hängen
   (Bootgrund danach `reboot,longkey,master_dc`).
6. Messmodul für die PMIC-Leistungsmesser (`kernel/odpm`) geschrieben; lief auf
   Anhieb, Abtastrate plausibel (ca. 129 Hz bei nominal 125). Nicht gegengeprüft:
   Skalierung der Shunt-Kanäle (Modem, Display) und der optionale DSM-Trim.
7. Schnelltests ohne Wirkung auf den Grundverbrauch: ModemManager stoppen,
   CPU 4–7 offline, Greeter stoppen, Funk auf „low".
8. Zustand am Ende: Telefon läuft normal, `sicd=0`, WLAN-Modul und Sensor-Dienst
   wieder aktiv, Akku 88 %.
