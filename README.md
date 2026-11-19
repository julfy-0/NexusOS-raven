# NexusOS (raven — Google Pixel 6 Pro Port)

> 📱 **Note**: This repository is a dedicated bare-metal AArch64 port for the **Google Pixel 6 Pro ("raven", Tensor GS101)**, based on the upstream core [NexusOS](https://github.com/julfy-0/NexusOS) operating system.

NexusOS is a freestanding custom operating system running directly on bare metal without any Android runtime, Bionic, or Linux kernel.

---

## Quick Start

### Requirements

- WSL/Linux with `aarch64-linux-gnu-gcc` (cross toolchain)
- Google Pixel 6 Pro with an **unlocked bootloader**
- `fastboot` / `adb`

### Build Standalone Boot Image

```bash
./build.sh
```
*(or `bash ./build.sh`)*

The resulting bootable image will be generated at `build/pixel6pro/boot.img`.

### Install to Slot B (Safe Dual-Boot)

```bash
# Flash to slot B boot partition (slot A Android remains untouched):
fastboot flash boot_b build/pixel6pro/boot.img

# Switch to slot B and boot NexusOS:
fastboot --set-active=b
fastboot reboot

# Return to Android at any time:
fastboot --set-active=a
fastboot reboot
```

> ⚠️ Save your stock boot image before experimenting: `fastboot fetch boot boot_stock.img` (or extract it from the full factory image).

For full documentation and port details, see [docs/PORT_PIXEL6PRO.md](docs/PORT_PIXEL6PRO.md) and [docs/XDA_THREAD_DRAFT.md](docs/XDA_THREAD_DRAFT.md).

---

## Documentation

See `docs/` for the port notes and architecture:

- [docs/PORT_PIXEL6PRO.md](docs/PORT_PIXEL6PRO.md) — port status, hardware facts and blockers
- [docs/PORT_DRIVERS_GUIDE.md](docs/PORT_DRIVERS_GUIDE.md) — driver porting guide
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — kernel architecture overview

---

## 🤝 Contributing

We welcome any contributions from the bare-metal and Android porting community! Whether it is driver development (touchscreen SPI, Wi-Fi PCIe, audio DSP), power management, reverse-engineering Tensor registers, or userspace apps:

👉 **Please check out our [Contributing Guide (CONTRIBUTING.md)](CONTRIBUTING.md)** for priority tasks, coding standards, and how to submit a Pull Request.