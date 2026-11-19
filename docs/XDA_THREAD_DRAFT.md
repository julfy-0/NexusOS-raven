# [DEV][WIP][ALPHA] NexusOS — Standalone Bare-Metal Operating System for Pixel 6 Pro (raven)

```
/*
 * Your warranty is now void.
 *
 * I am not responsible for bricked devices, dead storage, thermonuclear war,
 * or your phone alarm failing to wake you up. You choose to make these
 * modifications at your own risk.
 */
```

---

## 📌 Introduction

**NexusOS** is **not** an Android custom ROM, GSI, or Linux chroot.  
It is a **custom, freestanding, bare-metal operating system** written from scratch in C and assembly, running directly on hardware.

Originally created as an x86_64 UEFI OS, NexusOS has now received a native **AArch64 port** specifically tailored for the **Google Pixel 6 Pro ("raven", Google Tensor GS101)**. It boots directly via a standard Android `boot.img` passed by ABL (Android Bootloader).

---

## 🚀 Features & Current Status

NexusOS boots on bare metal without any Android runtime, Bionic, or Linux kernel under the hood.

### ✅ What's Working:
* **AArch64 Bootloader Handoff**: Standard `boot.img` (header v4). Automatic relocation, EL2 → EL1 switch, page tables (2MB granule identity map, Normal Cacheable + Non-Cacheable + Device MMIO).
* **Display (DPU / DECON0)**: Native resolution **1440 x 3120** AMOLED scanning. Direct rendering into framebuffer, boot console log, custom mobile UI with interactive App Dock and 5-row Touch OSK (On-Screen Keyboard).
* **Touchscreen (Samsung S6SY791 / SEC_TS)**: Full hardware communication over **SPI11 / USI11 (Mode 3, CPOL=1, CPHA=1)**. Multi-touch coordinate decoding (up to 10 points), visual touch ripple feedback.
* **Storage (UFS 3.1 & GPT Scanner)**:
  * Hardware UFS 3.1 HCI driver @ `0x14700000`.
  * Block abstraction layer translating 512B sectors into native 4K UFS blocks.
  * Real-time GPT partition table scanner: discovers and enumerates device partitions (`boot_a`, `system_a`, `userdata`, etc.).
* **USB & PHY (Synopsys DWC3 & Samsung USBDRD PHY)**:
  * Samsung USB 3.0 DRD PHY initialization and PMU line isolation clearing (`0x17463EB0`/`0x3EB4`).
  * Synopsys DWC3 USB controller running in Device mode (USB Mass Storage exposing UFS) with xHCI host discovery.
* **Hardware Watchdog (WDT)**: Dual-cluster GS101 Watchdog timers (`0x10060000` / `0x10070000`) disabled and kept alive (`wdt_pet()`) to eliminate automatic reboot cycles.
* **Buttons & Power Management**: Hardware buttons (Power `GPA10[1]`, Volume Down `GPA7[3]`, Volume Up `GPA8[1]`) with instant on-screen HUD, long-press power-off via PSCI, and combo detection.
* **Interrupt Controller (ARM GICv3)**: Distributor (`0x10400000`), CPU0 Redistributor (`0x10440000`), and AArch64 CPU Interface system registers configured.

### ⏳ Work In Progress / Not Working Yet:
* Wi-Fi & Bluetooth (Broadcom PCIe/UART)
* Audio (DSP / Sliders)
* Camera (Titan M2 / ISP)
* Deep sleep / ACPM CPU DVFS frequency scaling

---

## 📲 How to Install & Dual-Boot (Slot B Isolation)

> 💡 **SAFE DUAL-BOOT**: Google Pixel 6 Pro uses an A/B seamless update partition scheme. NexusOS is installed exclusively into **Slot B's boot partition (`boot_b`)**.  
> Your daily Android OS, apps, photos, and userdata live on **Slot A** and remain **100% untouched and safe**. You can switch back to Android at any moment!

### Prerequisites:
1. Google Pixel 6 Pro ("raven") with **Unlocked Bootloader** (`fastboot flashing unlock`).
2. USB Type-C cable and Google Platform Tools (`fastboot` and `adb`).
3. Ensure your main Android system is running on **Slot A** (default on most setups, or check with `fastboot getvar current-slot`).

---

### Step 1: Flash to Boot Partition of Slot B
1. Reboot phone into Fastboot mode:
   ```bash
   adb reboot bootloader
   # OR hold Power + Volume Down while powered off
   ```
2. Flash `boot.img` directly to the `boot_b` partition:
   ```bash
   fastboot flash boot_b boot.img
   ```

---

### Step 2: Boot NexusOS (Switch to Slot B)
1. Set the active slot to B and reboot:
   ```bash
   fastboot --set-active=b
   fastboot reboot
   ```
2. **NexusOS will boot!**
   * You will see the colored hardware probing console (`[ OK ]`).
   * The mobile UI (wallpaper, app dock, and touch on-screen keyboard) will appear.
   * Tap the screen to test touch ripple feedback or press physical volume/power buttons.

---

### 🔄 How to Return to Android (Switch Back to Slot A)
Whenever you want to go back to your normal Android system:
1. Reboot into Fastboot mode (hold **Power + Volume Down**).
2. Set the active slot back to A and reboot:
   ```bash
   fastboot --set-active=a
   fastboot reboot
   ```
3. Your device will boot right back into your normal Android system with all data intact.

---

## 🛠️ Building From Source

```bash
# Required: aarch64-linux-gnu-gcc cross compiler & python3
git clone https://github.com/julfy-0/NexusOS-raven.git
cd NexusOS-raven

# Build standalone bootable boot.img for Pixel 6 Pro:
make -f Makefile.pixel6pro clean
make -f Makefile.pixel6pro -j$(nproc)
```
The resulting image will be generated at `build/pixel6pro/boot.img`.

---

## 🔗 Sources & Credits

* **GitHub Repository**: [julfy-0/NexusOS-raven](https://github.com/julfy-0/NexusOS-raven)
* **Author / Developer**: @julfy-0
* **Platform Trees & References**: Google Android Open Source Project (`android-gs-raviole`), Linux upstream Exynos drivers.

---

**Thread Tags**: `pixel 6 pro`, `raven`, `nexusos`, `kernel`, `bare-metal`, `development`, `gs101`, `tensor`, `dual-boot`
