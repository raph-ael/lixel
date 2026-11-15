# lixel – natives Linux auf dem Pixel 6 (oriole)

Ziel: Das Pixel 6 (Tensor gs101) als normal nutzbares Linux-Telefon betreiben –
mit funktionierendem Standby, WLAN, Mobilfunk, Anrufen und SMS. Bevorzugt später
auf Debian-Basis; aktuell läuft postmarketOS aus dem Fork
[TheNoswad/pixel6-mainline-nura](https://github.com/TheNoswad/pixel6-mainline-nura)
mit eigenen Kernel-Änderungen.

- **[docs/STATUS.md](docs/STATUS.md)** – aktueller Stand, Messwerte, offene Punkte, nächste Schritte
- **[docs/PROTOKOLL.md](docs/PROTOKOLL.md)** – was in welcher Reihenfolge gemacht wurde, inkl. Fehlern und Lösungen
- **[docs/SETUP.md](docs/SETUP.md)** – Arbeitsumgebung wiederherstellen, Kernel bauen und aufspielen, Notfall-Wiederherstellung

## Inhalt

| Pfad | Zweck |
|---|---|
| `kernel/build.sh`, `kernel/deploy.sh` | Kernel auf dem Rechner bauen und über USB aufs Telefon spielen |
| `kernel/patches/` | eigene Kernel-Änderungen (auf den Fork-Kernel) und Versionsanpassungen an pmaports |
| `kernel/odpm/` | Kernelmodul für die Leistungsmessung pro Versorgungsschiene |
| `kernel/tools/gen-sleep-seq.py` | erzeugt die Schlafsequenzen aus Googles Herstellerkernel |
| `tools/` | SSH-Helfer und Messskript fürs Telefon |
| `build-debian.sh` | baut ein Debian-13-Rootfs (Machbarkeitsnachweis, bootete bereits) |
| `pmb-nura.cfg` | pmbootstrap-Konfiguration für den Fork |
| `logs/` | Build- und Bootloader-Protokolle |
| `docs/memory/` | Arbeitsnotizen des Assistenten |

Große Fremdquellen (Kernelbaum, pmaports, Herstellerkernel, Images) liegen nicht
im Repo; `docs/SETUP.md` beschreibt, wie man sie wiederherstellt.

