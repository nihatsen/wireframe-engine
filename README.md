# wireframe-engine
Whole new level of dark mode for your phone 🤭🤭

<img width="359" height="168" alt="Screenshot_20260406-180637_Brawl Stars" src="https://github.com/user-attachments/assets/fef0610f-df97-44a7-88fe-7b27f42d7860" />
<img width="359" height="168" alt="image" src="https://github.com/user-attachments/assets/62cacbb9-a978-4226-a651-2c6ea9bff726" />
<img width="135" height="295" alt="Screenshot_20260406-172042_YouTube Music" src="https://github.com/user-attachments/assets/6827fb3e-e895-4543-8f3c-89db916628e9" />
<img width="135" height="295" alt="Screenshot_20260407-195528_Pinterest" src="https://github.com/user-attachments/assets/854bea8f-2362-4c58-8e29-d84d83d57ec1" />
<img width="135" height="295" alt="Screenshot_20260407-200203_Claude" src="https://github.com/user-attachments/assets/9238aac3-f952-41bb-94c5-c951e6086a78" />
<img width="135" height="295" alt="Screenshot_20260407-224107_Instagram" src="https://github.com/user-attachments/assets/2eaeccf0-1387-4c63-8f3a-4d9f458127fe" />
<img width="135" height="295" alt="Screenshot_20260407-222323_YouTube" src="https://github.com/user-attachments/assets/b45ded0a-430d-44cf-98cb-4efc26297c85" />


THIS MODULE TURNS YOUR APPS INTO WIREFRAMES!!!! So you can enjoy games, videos, any application at night.. your eyes may rest now 😇😇

Used Sobel Edge algorithm for this module.

Dependencies; 
ZygiskNext, depth of your heart.

Brawl stars gameplay with this module: https://youtu.be/-Em6uf7pj30
Instagram test: https://youtu.be/tG_CgPhra0g

GPUs, driver and renderers might change by device and android version...
Well, just make it compatible with your own phone. 

Tested on: Xiaomi Redmi Note 8, 
ROM: Project infinity X Android 16. 
Kernel: Floppy_v1.2-KSUNext-NOSUS-mitrinket-20251229-0532, 
Installed this module via KernelSU-Next.

Since my other phone has been broken for almost half a year, I have not been able to test on samsung phones.
Even so, you would have to modify this module to work on your specific device model.


All from my heart to; you.
I am waiting for your contributions to this module.

💖💖💖💖




# ⬇️ AI GENERATED README FILE ⬇️
# 🔲 Wireframe Render Engine

> System-wide GPU-accelerated wireframe rendering for Android — the darkest dark mode you've never seen.

A Zygisk module that hooks directly into the Android graphics pipeline to transform every surface on your screen — games, videos, apps, SystemUI — into a real-time wireframe. Uses GPU-accelerated edge detection shaders, requires no per-app setup, and works system-wide at full frame rate.

📺 **[Brawl Stars gameplay demo](https://youtu.be/-Em6uf7pj30)**
📺 **[Instagram demo](https://youtu.be/tG_CgPhra0g)**
---

## How It Works

The module injects a native shared library via Zygisk into every app process at startup. It hooks `eglSwapBuffers` to intercept each rendered frame before it hits the display, runs the frame through a GLSL edge-detection shader on the GPU, and writes the result back — all in the same frame budget.

The GLSL shader is compiled to SPIR-V and embedded at build time, so there is no runtime shader compilation delay. Config changes are picked up within ~1 second via file-watch without requiring a reboot.

---

## Features

### Render Modes
| Mode | Description |
|------|-------------|
| `Wireframe` | White edges on pure black — maximum contrast |
| `Overlay` | Edge lines drawn over the original image |
| `Inverted` | Black edges on white |
| `Colored` | Edges tinted with the original pixel color |
| `X-Ray` | Transparent background + edge lines |
| `Game` | Multi-channel detection that catches color boundaries luminance misses — tuned for 3D games |

### Edge Detection Algorithms
| Algorithm | Texture Lookups | Notes |
|-----------|----------------|-------|
| Roberts Cross | 4 | Fastest |
| Sobel | 8 | Default — balanced quality/performance |
| Scharr | 8 | Higher quality than Sobel |
| Frei-Chen | 9+ | Best quality, most GPU cost |

### Configuration
- **Live editing** — edit `/data/local/tmp/wireframe/config.conf` and changes apply within ~1 second
- **Web UI** — served via KernelSU's WebUI module interface for visual control
- **Per-app profiles** — override any setting for specific packages (e.g. separate tuning for a game)
- **Scope control** — include or exclude SystemUI, launcher, keyboard, and arbitrary packages via whitelist/blacklist
- **Quality presets** — Performance / Balanced / Quality / Ultra

### Performance Knobs
- Processing resolution scale (25%–100%)
- Frame skip
- Compute shader path (GLES 3.1+)
- Async framebuffer copy via PBO (reduces GPU pipeline stalls at cost of 1 frame latency)
- Half-float textures (`RGBA16F`)
- Compiled shader binary cache

### Platform Support
- **Architectures:** `arm64-v8a`, `armeabi-v7a`
- **Root:** Magisk (with Zygisk enabled) or KernelSU + ZygiskNext
- **GPU:** Any OpenGL ES 3.0+ GPU; Adreno-specific optimizations for Snapdragon 665/Adreno 610 devices

---

## Requirements

- Rooted Android device
- **Magisk** with Zygisk enabled, **or** **KernelSU** with [ZygiskNext](https://github.com/Dr-TSNG/ZygiskNext) installed
- Android NDK `26.1.10909125` (for building from source)

---

## Installation

### Pre-built ZIP (recommended)

1. Download the latest `wireframe-engine-vX.X.X.zip` from [Releases](../../releases)
2. Flash the ZIP via Magisk / KernelSU module installer
3. Reboot

> **KernelSU users:** Install ZygiskNext first, then install the Wireframe Engine module.

### Windows one-click installer (via ADB)

If you have ADB set up and the device connected via USB:

```
install_modules.bat
```

This script pushes both ZygiskNext and Wireframe Engine to the device and installs them via `ksud`, then optionally reboots.

---

## Building from Source

### Prerequisites

- Android NDK — set `ANDROID_NDK_HOME` or `ANDROID_NDK`, or install via:
  ```bash
  sdkmanager --install 'ndk;26.1.10909125'
  ```

### Build

```bash
./build.sh
```

The script will:
1. Auto-detect your NDK installation
2. Download `zygisk.hpp` from the Magisk repo if not present
3. Compile native libraries for `arm64-v8a` and `armeabi-v7a`
4. Package everything into a flashable ZIP at `out/wireframe-engine-vX.X.X-YYYYMMDD.zip`

### Quick repack (config changes only, no recompile)

```bash
./pack.sh
```

---

## Configuration

The config file lives at two locations:
- **Runtime (live):** `/data/local/tmp/wireframe/config.conf` — edit this for instant changes
- **Persistent:** `/data/adb/wireframe/config.conf` — survives reboots and `/data/local/tmp` clears

Changes to the runtime config are automatically synced to the persistent copy.

### Key settings

```ini
# Master on/off
enabled=1

# Render mode (1=Wireframe, 2=Overlay, 3=Inverted, 4=Colored, 5=X-Ray, 6=Game)
mode=1

# Edge detection algorithm (0=Roberts, 1=Sobel, 2=Scharr, 3=Frei-Chen)
shader_quality=1

# Edge sensitivity — lower catches more edges
edge_threshold=0.08

# Line and background colors (RGB 0.0–1.0)
line_color_r=1.0
line_color_g=1.0
line_color_b=1.0
bg_color_r=0.0
bg_color_g=0.0
bg_color_b=0.0
```

### Per-app profiles

```ini
[profile.com.example.mygame]
mode=6
edge_threshold=0.06
line_color_r=0.0
line_color_g=1.0
line_color_b=0.4
line_intensity=1.5
```

Apps with a profile entry are automatically whitelisted regardless of the global `apply_all` setting.

### Runtime toggle (no config edit needed)

```bash
# Disable
adb shell setprop persist.wireframe.enabled 0

# Enable
adb shell setprop persist.wireframe.enabled 1
```

### Debug logging

```bash
adb shell setprop persist.wireframe.debug 1
# Logs appear at /data/local/tmp/wireframe/engine.log
```

---

## Project Structure

```
wireframe-engine/
├── jni/                    # Native C++ source + Android.mk / Application.mk
│   └── zygisk.hpp          # Downloaded at build time from Magisk
├── module/
│   ├── zygisk/             # Compiled .so libraries (populated by build.sh)
│   ├── system/etc/wireframe/
│   │   └── config.conf     # Default configuration shipped with the module
│   ├── webroot/
│   │   └── index.html      # KernelSU Web UI
│   ├── customize.sh        # Install-time setup (arch check, permissions, GPU detection)
│   ├── post-fs-data.sh     # Early boot: config seeding, Adreno governor hints
│   ├── service.sh          # Late boot: config file watcher, GPU debug monitor
│   ├── uninstall.sh        # Cleanup on module removal
│   ├── sepolicy.rule       # SELinux policy additions
│   └── module.prop
├── build.sh                # Full build script (NDK → .so → flashable ZIP)
├── pack.sh                 # Repack module ZIP without recompiling
└── install_modules.bat     # Windows ADB installer
```

---

## License

[MIT](LICENSE)
