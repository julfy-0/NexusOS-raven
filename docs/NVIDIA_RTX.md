# NVIDIA RTX support

NexusOS claims **RTX-generation** NVIDIA GPUs only: Turing RTX (RTX 20 / Quadro RTX / TITAN RTX), Ampere (RTX 30 / RTX A), Ada Lovelace (RTX 40) and Blackwell (RTX 50). GTX 16/10, GT, MX, Hopper and compute-only boards are left alone on the UEFI GOP framebuffer.

## How a GPU is accepted

1. Every PCI class 03 (subclass 00 VGA or 02 3D) device with vendor `0x10DE` is considered, not just the first display controller.
2. The PCI device ID selects a generation window (`nvidia_gpu_rtx_arch`). Known boards inside a window that are not RTX are excluded explicitly.
3. PCI memory decode is enabled, 2 MiB of BAR0 is mapped uncached and the chip ID (`PMC_BOOT_0`, falling back to `BOOT_42`) is read. The chipset family must match the generation (TU116/TU117 = GTX 16, GA100 and Hopper are rejected). If the register cannot be read the GPU is accepted by ID only and `gpuinfo` says so.

The driver is **read-only**: it never writes GPU registers.

## Refresh rate: Hz is not FPS

- *Refresh rate (Hz)* is how often the GPU scans the screen out to the monitor. Today the UEFI firmware chooses it from the monitor's EDID; NexusOS reads that timing (`display_panel_refresh_hz`) and the panel's advertised maximum (`display_panel_max_refresh_hz`). The EDID reader now copies and parses up to four 128-byte blocks, including CTA-861 extension detailed timings; 144 Hz modes listed only in an extension are no longer silently missed.
- *Frame limit (FPS cap)* in Settings only paces how often NexusOS renders. A 144 cap on a 60 Hz scan-out shows 60 distinct frames per second at best.
- If a 144 Hz panel is driven at 60 Hz, Settings > Display and `gpuinfo` say so. To get 144 Hz on the current GOP-only graphics path, the firmware must already pick that timing: select the 144 Hz mode in firmware/monitor setup if offered. EDID detection can report 144 Hz support, but NexusOS cannot yet switch scan-out itself.

## What is missing for real Hz switching and acceleration

On Turing and newer the display engine, power management and every engine (graphics, copy, NVDEC, NVENC) are brought up by the **GSP-RM firmware** (signed, supplied by NVIDIA) and driven through NVIDIA's resource-manager interface (see NVIDIA's `open-gpu-kernel-modules`). Without it a kernel cannot set a display mode, so `display_set_refresh_hz()` returns `DISPLAY_ERR_UNSUPPORTED`. Video decode/encode additionally needs a media stack on top; NexusOS has no video player yet.

Software-side graphics speed that already exists: write-combining framebuffer mapping (PAT), back buffer with a single burst copy (`rep movsq`), TSC-accurate frame pacing, and dirty-rectangle pointer updates.

## Hooking in a real backend later

Implement mode setting in `nvidia_rtx.c` (`nvidia_rtx_modeset_available()` -> 1) and perform the switch in `display_set_refresh_hz()`; Settings and `gpuinfo` pick it up automatically.
