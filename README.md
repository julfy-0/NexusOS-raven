# NexusOS (raven — Google Pixel 6 Pro Port)

> 📱 **Note**: This repository is a dedicated bare-metal AArch64 port for the **Google Pixel 6 Pro ("raven", Tensor GS101)**, based on the upstream core [NexusOS](https://github.com/julfy-0/NexusOS) operating system.

NexusOS is a freestanding custom operating system running directly on bare metal without any Android runtime, Bionic, or Linux kernel.

---

## Google Pixel 6 Pro (raven) Quick Start

### Build Standalone Boot Image:
From WSL/Linux:
```bash
./build.sh
```
*(or `bash ./build.sh`)*

The resulting bootable image will be generated at `build/pixel6pro/boot.img`.

### Install to Slot B (Safe Dual-Boot):
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

For full documentation and port details, see [docs/PORT_PIXEL6PRO.md](docs/PORT_PIXEL6PRO.md) and [docs/XDA_THREAD_DRAFT.md](docs/XDA_THREAD_DRAFT.md).

---

## 🤝 Contributing

We welcome any contributions from the bare-metal and Android porting community! Whether it is driver development (touchscreen SPI, Wi-Fi PCIe, audio DSP), power management, reverse-engineering Tensor registers, or userspace apps:

👉 **Please check out our [Contributing Guide (CONTRIBUTING.md)](CONTRIBUTING.md)** for priority tasks, coding standards, and how to submit a Pull Request.

---

## Upstream NexusOS Core

NexusOS was originally designed as a freestanding x86_64 operating-system project built around a UEFI boot path, a monolithic kernel, hardware drivers, a command-line shell and a framebuffer desktop.

## Current release

**NexusOS 0.7 — Enstein**

The current milestone is **NexusOS 0.7 — Native Userspace Runtime**. This release begins the Nexus Native Architecture with a public syscall ABI, Nexus Runtime service, and cross-process Nexus Channel IPC while preserving the 0.6.x stability work.

## Build

From WSL/Linux with the required cross/freestanding toolchain installed:

```bash
make clean
make -j2 iso
make check
```

The bootable ISO is produced at `build/NexusOS-0.7.iso`.

For a standalone ISO from an already-built tree:

```bash
./build_pc.sh
```

## Project documentation

See `docs/` for architecture, building, networking, shell I/O, filesystem, release, roadmap and the 0.7 native runtime foundation.

The Native Runtime userspace library also provides `runtime_context.h`/`runtime_context.c` for reusable runtime state snapshots. Build it with `make user-runtime`.


### NexusOS 0.7

The 0.7 line begins the Nexus Native Architecture. The 0.6.x line is treated as the stability foundation.

System font assets: `assets/fonts/Roboto-Regular.ttf` and `assets/fonts/Roboto-Bold.ttf`.

## Build and image creation

The main build command now also creates the disk image and standalone ISO:

```bash
./build_pc.sh
```

Custom total image sizes are supported:

```bash
./build_pc.sh --size 1G
./build_pc.sh --size 16G
./build_pc.sh --size 1T
```

To rebuild only the image/media from existing build artifacts:

```bash
./build_pc.sh --image-only --size 4G
```

`NexusOS.img` and `NexusOS.iso` are created in the project root by default.


### 0.6.0 Display modes and framebuffer
NexusOS enumerates UEFI GOP modes and prefers the largest available pixel-area mode. The GUI can render into a kernel backbuffer and present it to the firmware framebuffer. UEFI GOP basic mode information does not expose refresh rate; exact arbitrary-Hz switching remains GPU-driver work (see `docs/NVIDIA_RTX.md`). The GUI "frame limit" is an FPS cap; the real refresh rate shown in Settings > Display comes from the monitor EDID. EDID parsing includes CTA-861 extension detailed timings (where many 120/144 Hz modes are advertised), but detection is not the same as switching: a GPU-specific mode-setting backend is still required to change scan-out from 60 to 144 Hz.
