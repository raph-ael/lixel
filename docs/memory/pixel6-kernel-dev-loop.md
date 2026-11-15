---
name: pixel6-kernel-dev-loop
description: How to build/deploy the Pixel 6 (oriole) Linux kernel from the host and recover a failed boot
metadata:
  node_type: memory
  type: project
  originSessionId: e9ca16c2-7ea5-47f6-9300-eff581b81017
  modified: 2026-11-15T08:03:27.998Z
---

Pixel 6 "lixel" kernel work (started 2026-11-15) uses a host-side loop, not pmbootstrap:

- Source: `~/orca/projects/lixel/kernel/linux` (M8 gs101-mainline 7.2.0-rc3 + fork patches, local git). Build: `kernel/build.sh` (clang, ~5 min incremental). Deploy over USB network: `kernel/deploy.sh [reboot]` (root SSH key on phone, 172.16.42.1; writes boot+vendor_boot of BOTH slots).
- Vendor reference (Google GPL kernel): `~/orca/projects/lixel/ref/gs` (cal-if/gs101 = PMU sequences).
- Known-good boot images: `kernel/boot-good/` (flash with fastboot if a kernel does not boot).
- Debug files on phone: `/sys/kernel/debug/gs101-pmu/{regs,flexpmu,blk}`, params in `/sys/module/exynos_pmu/parameters/`, battery-forcing switch `.../max77759-charger.*/force_battery`.

**Why:** pmbootstrap kernel builds take ~1 h; deep-suspend/power work needs many iterations.

**How to apply:**
- Nothing marks the A/B slot successful, so the bootloader's retry counter runs out after ~3 boots and it falls back to the other slot ("GrapheneOS Recovery – cannot load Android" appeared once). Never factory-reset there (wipes the Linux rootfs in userdata); `adb reboot bootloader` + `fastboot set_active b` fixes it. Both slots now carry Linux.
- Do not read APM SRAM (0x2039000) via /dev/mem or scan it: parts abort and reset the phone.
- Suspend tests need `systemctl stop oriole-sensorproxy` (USF wake lock) and `modprobe -r bcmdhd` first.

Power findings (2026-11-15, measured with `kernel/odpm/pixel6-odpm.ko` → `/sys/kernel/debug/odpm/{power,assign}` and `/root/pwr.sh` on the phone):
- Idle with display off draws ~2.3 W (550–700 mA); CPUs are only ~55 mW. Long tail: modem bursts (76–620 mW, it does sleep ~90 %), PCIe1 PHY LDOs 127 mW constant, USB PHY 110 mW (cable), display VSYS 142 mW with panel off, MIF 176 mW (stuck at 2028 MHz, no devfreq), G3D 94 mW, IO rails.
- Powering off unused blocks (`echo 'MFC 0' > /sys/kernel/debug/gs101-pmu/blk`, also G2D DNS G3AA IPP ITP MCSC GDC TNR BO TPU) works but saves nothing measurable.
- s2idle + SICD (`echo 1 > /sys/module/exynos_pmu/parameters/sicd`, read debugfs `flexpmu` once first) really powers the SoC down and resumes (apsoc_sicd counter rises). Open: modem IRQ 177 (cp2ap_wakeup) ends s2idle after 2–4 s; hundreds of aborted SICD entries with WAKEUP_STAT 0; MIF never goes down (mif_sicd 0).
- Deep suspend (PSCI SYSTEM_SUSPEND) with vendor enter/exit sequences enters but never resumes; needs 30 s power-key hold. pm_test=core passes.
- ModemManager polls SIT 0x0602 every 5 s (bearer status) — keeps waking the modem link while awake.
