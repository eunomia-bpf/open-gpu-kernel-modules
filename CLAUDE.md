# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

This is the NVIDIA Linux open GPU kernel module source code. The repository contains open-source kernel modules for NVIDIA GPUs (Turing and later), version 580.95.05. These modules work with GSP firmware and must be paired with corresponding user-space drivers.

## Build Commands

### Building
```bash
# Build all kernel modules
make modules -j$(nproc)

# Build with verbose output
make modules -j$(nproc) NV_VERBOSE=1

# Build with debug symbols and logging
make modules -j$(nproc) DEBUG=1
```

### Installation
```bash
# Install kernel modules (requires root)
make modules_install -j$(nproc)
```

### Cross-compilation
```bash
# Example: compile on x86_64 for aarch64
make modules -j$(nproc) \
    TARGET_ARCH=aarch64 \
    CC=aarch64-linux-gnu-gcc \
    LD=aarch64-linux-gnu-ld \
    AR=aarch64-linux-gnu-ar \
    CXX=aarch64-linux-gnu-g++ \
    OBJCOPY=aarch64-linux-gnu-objcopy
```

### Cleaning
```bash
make clean
```

## Architecture

### Two-Tier Module Structure

Each kernel module (except nvidia-drm and nvidia-uvm) is split into two components:

1. **OS-Agnostic Component** (`src/`): Platform-independent GPU driver logic
   - `src/nvidia/` - Core GPU driver (nvidia.ko)
   - `src/nvidia-modeset/` - Display mode-setting (nvidia-modeset.ko)
   - `src/common/` - Shared utility code
   - Built first, produces `.o` object files

2. **Kernel Interface Layer** (`kernel-open/`): Linux kernel-specific interfaces
   - `kernel-open/nvidia/` - Interface for nvidia.ko
   - `kernel-open/nvidia-drm/` - DRM driver (nvidia-drm.ko)
   - `kernel-open/nvidia-modeset/` - Interface for nvidia-modeset.ko
   - `kernel-open/nvidia-uvm/` - Unified Virtual Memory (nvidia-uvm.ko)
   - `kernel-open/nvidia-peermem/` - Peer memory support (nvidia-peermem.ko)
   - Built second using Kbuild, links with OS-agnostic components

### Build Process

1. Top-level Makefile builds OS-agnostic components in `src/`
2. Creates symbolic links to binary objects in `kernel-open/`
3. Invokes `kernel-open/Makefile` which uses Linux Kbuild system
4. Kbuild produces final `.ko` kernel modules

### Key Kernel Modules

- **nvidia.ko**: Main GPU driver, handles GPU initialization, memory management, and hardware access
- **nvidia-modeset.ko**: Display mode-setting and configuration
- **nvidia-drm.ko**: DRM (Direct Rendering Manager) integration for display
- **nvidia-uvm.ko**: Unified Virtual Memory for CUDA applications
- **nvidia-peermem.ko**: Peer memory access for GPU-to-GPU communication

## Supported Configurations

- **Architectures**: x86_64, aarch64
- **Kernel versions**: Linux 4.15 or newer
- **Toolchains**: GCC or Clang (must match kernel build toolchain)
- **GPU generations**: Turing (RTX 20 series) and newer

## Integration Notes

### Nouveau Driver
The `nouveau/` directory contains tools for extracting firmware binary images from source code. These are used by the open-source Nouveau driver for GSP firmware loading.

### Build Variables
- `NV_VERBOSE=1`: Print full compilation commands
- `DEBUG=1`: Enable debug builds with logging
- `TARGET_ARCH`: Set target architecture (x86_64, aarch64)
- `NV_EXCLUDE_KERNEL_MODULES`: Exclude specific modules from build

## Contributing

- No cosmetic/non-functional changes (typos, whitespace, comments)
- Focus on executable code improvements only
- Large refactoring changes should be coordinated with NVIDIA in advance
- Code style: Match surrounding code in the repository
- Pull requests require accepting a Contributor License Agreement

## Important Notes

- This codebase is shared with NVIDIA's proprietary drivers
- The GitHub repository functions as a snapshot per driver release
- Individual contribution commits may not be preserved due to internal processing
- Built modules must match user-space driver version (580.95.05)
