# Contributing to NexusOS-raven

First off, thank you for considering contributing to **NexusOS-raven**!  
NexusOS is an ambitious bare-metal operating system project running directly on Google Pixel 6 Pro ("raven", Tensor GS101) without Linux or Android runtime layers.

Community contributions, driver reverse-engineering, hardware testing, and bug fixes are warmly welcome!

---

## 🎯 Priority Areas Where Help is Needed

If you are looking for things to work on, here are our current top priorities:

1. **Touchscreen Driver (Samsung S6SY791 / SEC_TS)**:
   - File: `kernel/arch/aarch64/touch_raven.c` & `spi_gs101.c`
   - Goal: Stable SPI11 transfer handshakes, interrupt line GPIO handling, coordinate and multi-touch report decoding.
2. **Wi-Fi & Bluetooth**:
   - Broadcom BCM4389 PCIe / UART integration and firmware loading.
3. **Power Management & PMIC**:
   - ACPM IPC protocols, DVFS CPU frequency scaling, battery fuel gauge reading.
4. **Audio Subsystem**:
   - Sound Open Firmware (SOF) / DSP codec bring-up.
5. **Userspace & UI Applications**:
   - Enhancing the mobile compositor (`gui/mobile_ui.c`), adding apps, on-screen keyboard layouts, and widgets.

---

## 🛠️ Development Workflow

### 1. Fork and Clone
1. Fork the repository on GitHub: [https://github.com/julfy-0/NexusOS-raven](https://github.com/julfy-0/NexusOS-raven)
2. Clone your personal fork:
   ```bash
   git clone https://github.com/<your-username>/NexusOS-raven.git
   cd NexusOS-raven
   ```

### 2. Create a Topic Branch
Always develop features or fixes on a dedicated branch rather than `main`:
```bash
git checkout -b feature/touch-spi-fix
# OR
git checkout -b fix/ufs-descriptor-offset
```

### 3. Build & Verify
Ensure you have `aarch64-linux-gnu-gcc` and `python3` installed.
```bash
# Build boot.img:
./build.sh
```
Your code **must compile cleanly with 0 errors and 0 warnings**.

### 4. Test on Hardware (Safe Slot B Dual-Boot)
Always flash only to the **secondary slot B** to protect your daily Android OS:
```bash
# Flash to slot B:
fastboot flash boot_b build/pixel6pro/boot.img

# Switch to slot B and boot NexusOS:
fastboot --set-active=b
fastboot reboot
```
To return to your normal Android system at any time:
```bash
fastboot --set-active=a
fastboot reboot
```

### 5. Commit Guidelines
Write descriptive and structured commit messages (following conventional commits):
```
feat(touch): add interrupt trigger configuration for SEC_TS
fix(ufs): correct response timeout handling in UFS HCI
docs(readme): update build instructions for Ubuntu 24.04
```

### 6. Submit a Pull Request
1. Push your branch to GitHub:
   ```bash
   git push -u origin feature/touch-spi-fix
   ```
2. Open a **Pull Request (PR)** against the `main` branch of `julfy-0/NexusOS-raven`.
3. In the PR description:
   - Explain what problem was solved or what feature was introduced.
   - Mention whether you tested it on hardware or in an emulator.
   - Attach photos or serial/Ramoops logs if applicable.

---

## 📝 Coding Standards

* **Language**: Freestanding C11 and AArch64 assembly.
* **No Host libc**: Do not `#include <stdio.h>` or standard POSIX headers. Use NexusOS core headers (`include/nexus/...`).
* **Hardware Register Access**: Use `mmio_read32()` / `mmio_write32()` macros with memory barriers (`dsb sy`, `isb`) where appropriate.
* **Documentation**: Document magic register offsets and bitfields with comments pointing to Samsung/Exynos/Tensor datasheet definitions or Linux upstream DTS bindings.
* **Safety**: Never write to partitions belonging to Slot A (`boot_a`, `system_a`, `userdata`) without explicit safety guards.

---

## 💬 Questions & Community

Have questions or want to discuss low-level Tensor architecture?
- Open a GitHub Issue or Discussion.
- Join the discussion on XDA Developers in the NexusOS Pixel 6 Pro thread.
