# Stand (15.11.2026)

## Was läuft

- Bootloader entsperrt, postmarketOS (Fork „Nura") mit Phosh bootet aus beiden Slots.
- Kernel `7.2.0-rc3+` (M8 gs101-mainline + 38 Fork-Patches + eigene Änderungen).
- Display 1080×2400 @ 90 Hz, GPU-Rendering (Panfrost, gepatchtes Mesa 26.2.6-r102), Bedienung flüssig.
- WLAN-Treiber findet Netze (noch mit keinem Netz verbunden).
- Mobilfunk: eingebucht bei 1&1 (LTE), mobile Daten über NM-Verbindung `mobil` (APN `internet`).
- Laden am PC-Port.
- Ein eigenes Debian-13-Rootfs (`build-debian.sh`) hat mit dem offiziellen pmOS-Kernel gebootet (Machbarkeitsnachweis; liegt nicht mehr auf dem Gerät).

## Ungetestet

- Anrufe und SMS (ein- und ausgehend). Im Modem-Log stand `carrier config status 0` und keine IMS-Registrierung – VoLTE bei 1&1 ist fraglich.
- Kameras, Bluetooth, GPS.

## Standby – Kern des Problems

Im Leerlauf mit dunklem Display zieht das Telefon ca. 2,3 W (550–700 mA am Akku).
Verteilung laut Messung pro Schiene (`kernel/odpm`):

| Verbraucher | Leistung |
|---|---|
| Modem (`VSYS_PWR_MODEM`) | 76–620 mW; schläft ca. 90 % der Zeit, zieht in Schüben viel |
| Speicher/Bus: `VDD_MIF` 176, `VDD2H_MEM` 75, `VDDQ_MEM` 30, `VDD_INT` 60 | ca. 340 mW; MIF steht fest auf 2028 MHz, es gibt kein devfreq |
| Display-Versorgung (`VSYS_PWR_DISPLAY`) trotz dunklem Panel | ca. 140 mW |
| `L17M`/`L19M_PCIE1` (PCIe-PHY, vermutlich WLAN), dauerhaft | ca. 127 mW |
| USB-PHY `L8M`/`L9M` (nur mit Kabel) | ca. 110 mW |
| GPU `VDD_G3D` im Leerlauf | ca. 94 mW |
| AoC | ca. 70 mW |
| CPU-Cluster zusammen | ca. 55 mW |
| Vorregler (`S9M_LLDO3` 350, `BB_HLDO` 315, `S7S_MLDO` 250) und IO-Schienen | Summen der daran hängenden LDOs |

Vor den Änderungen standen alle Schlafzähler der ACPM-Firmware auf 0: Der SoC
hat unter Linux nie abgeschaltet, auch nicht im s2idle des Forks. Es gibt keine
Power-Domains (alle Blöcke an), `clk_ignore_unused`/`regulator_ignore_unused`
stehen in der Kernel-Kommandozeile, CPU-Idle kennt nur C2 pro Kern.

### Eigene Kernel-Änderungen (`kernel/patches/0001-*.patch`)

1. **Tiefschlaf (SLEEP):** SLEEP-Hinweis in `CPU_INFORM`, `WAKEUP_INT_EN = 0x100bf`,
   Ein-/Austrittssequenzen aus Googles Herstellerkernel (`cal-if/gs101`), als
   syscore-Ops in `exynos-pmu.c`. Parameter `sleep_seq`, `sleep_hint`,
   `sleep_wakeup_en`, `sleep_exit_mode`.
2. **SICD im s2idle:** letzter Kern geht mit Hinweis 3 schlafen,
   `WAKEUP_INT_EN = 0x7f8f00ff`. Parameter `sicd` (0 aus = Standard, 1 nur im
   s2idle, 2 immer/experimentell). Braucht die ACPM-Zähler: vorher einmal
   `cat /sys/kernel/debug/gs101-pmu/flexpmu`.
3. **Debug:** `/sys/kernel/debug/gs101-pmu/regs` (Wake-Masken, Zustand aller
   Blöcke), `flexpmu` (Firmware-Zähler, Wake-Gründe), `blk` (Block schalten,
   z. B. `echo 'MFC 0'`).
4. **Ladetreiber:** `force_battery` (sysfs am max77759-charger) zwingt auf
   Akkubetrieb, damit man bei gestecktem USB messen kann.

### Ergebnisse

| Test | Ergebnis |
|---|---|
| Tiefschlaf mit `pm_test=core` | läuft komplett durch |
| Echter Tiefschlaf (`rtcwake -m mem`, `mem_sleep=deep`) | geht hinein, kommt nicht zurück; weder RTC noch Power-Taste wecken, 30 s Power nötig. Unklar, ob nur das Wecken fehlt oder das Resume hängt |
| s2idle ohne SICD | ca. 560 mA – spart nichts |
| s2idle mit `sicd=1` | SoC schaltet wirklich ab und kommt zurück (`apsoc_sicd` 6 pro Versuch), aber Ende nach 2–4 s durch IRQ 177 (`cp2ap_wakeup`, Modem) |
| 11 unbenutzte Blöcke aus (G2D MFC DNS G3AA IPP ITP MCSC GDC TNR BO TPU) | funktioniert stabil, spart nichts Messbares |

### Offene Punkte, in dieser Reihenfolge

1. **Modem weckt nach Sekunden.** Was es sendet, ist nicht identifiziert. In 22 s
   MM-Debug kamen keine SIT-Indications, nur die eigenen Abfragen. Kandidaten:
   Nachrichten auf `oem_ipc1` (Kernel meldet „oem_ipc1 is not opened", grob alle
   10 s) und fehlende „Bildschirm aus"-Meldung (`SIT_SET_IND_FILTER 0x0928` wird
   nur beim Einschalten mit `0xffffffff` gesetzt).
2. **ModemManager fragt alle 5 s `0x0602` ab** (Bearer-Status) – weckt die
   Modem-Verbindung im Wachzustand ständig. Im Shannon-Plugin abstellen.
3. **Hunderte abgebrochene SICD-Eintritte pro Sekunde** mit `WAKEUP_STAT = 0`;
   vollendete wurden durch Bit 19 (TIMER) und Bit 5 (EINT_FAR) geweckt.
4. **MIF geht nie herunter** (`mif_sicd` 0, „MIF req master" 0x3).
5. **Sensor-Dienst hält dauerhaft einen Wakelock** (`com.google.usf`, 14 Events/s);
   für Tests `systemctl stop oriole-sensorproxy`.
6. **bcmdhd verweigert das Suspend** bzw. überlebt es nicht; für Tests
   `modprobe -r bcmdhd` (der Fork macht das per sleep-Hook).
7. Danach: Verbrauch im abgeschalteten Zustand messen, Peripherie-Schienen im
   Suspend abschalten (PCIe-PHY, Display), MIF/INT-Takt senken.
8. Tiefschlaf-Resume untersuchen (zurückgestellt).
9. Anrufe/SMS testen; eingehender Anruf muss aus dem Standby wecken.
10. Später: Entscheidung Debian-Rootfs vs. postmarketOS; Phosh-Bedienung erklären;
    Root-SSH entfernen, Passwort ändern; Slot als „erfolgreich" markieren statt
    beide Slots zu spiegeln.

## Messen

Auf dem Telefon (root):

```
/root/pwr.sh 60 awake|mem          # mittlerer Akkustrom (tools/pwr.sh)
insmod /root/pixel6-odpm.ko
cat /sys/kernel/debug/odpm/power   # mW pro Schiene seit dem letzten Lesen
cat /sys/kernel/debug/odpm/assign  # Belegung; echo "3 LDO7M" > assign
```

Der Ladezähler `charge_counter` ist grob (ca. 1000 µAh-Schritte); für kurze
Fenster `current_now` mitteln.
