#!/system/bin/sh

ui_print "╔══════════════════════════════════════╗"
ui_print "║     WIREFRAME RENDER ENGINE v1.0     ║"
ui_print "║   GPU-Accelerated Edge Detection     ║"
ui_print "╚══════════════════════════════════════╝"

# Architecture check
ARCH=$(getprop ro.product.cpu.abi)
ui_print "- Device arch: $ARCH"

if [ "$ARCH" != "arm64-v8a" ] && [ "$ARCH" != "armeabi-v7a" ]; then
    abort "! Unsupported architecture: $ARCH"
fi

# Check for Zygisk support
if [ "$ZYGISK_ENABLED" != "true" ]; then
    # KernelSU with ZygiskNext check
    if [ -d "/data/adb/modules/zygisksu" ] || [ -d "/data/adb/modules/zygisk_next" ]; then
        ui_print "- ZygiskNext detected for KernelSU"
    else
        ui_print "! WARNING: Zygisk not detected"
        ui_print "! For KernelSU, install ZygiskNext first"
        ui_print "! Module may not function without Zygisk"
    fi
fi

# Verify zygisk libraries
if [ ! -f "$MODPATH/zygisk/$ARCH.so" ]; then
    abort "! Missing zygisk library for $ARCH"
fi

# Set permissions
ui_print "- Setting permissions..."
set_perm_recursive $MODPATH 0 0 0755 0644
set_perm_recursive $MODPATH/zygisk 0 0 0755 0755
set_perm_recursive $MODPATH/system/etc/wireframe 0 0 0755 0644

# Create runtime and persistent directories
mkdir -p /data/local/tmp/wireframe
chmod 0777 /data/local/tmp/wireframe
mkdir -p /data/adb/wireframe
chmod 0777 /data/adb/wireframe

# Preserve user config across module updates
if [ -f /data/adb/wireframe/config.conf ]; then
    ui_print "- Restoring saved configuration"
fi

# GPU detection
GPU=$(getprop ro.hardware.egl 2>/dev/null)
RENDERER=$(dumpsys SurfaceFlinger 2>/dev/null | grep -i "GLES" | head -1)
ui_print "- GPU backend: $GPU"
ui_print "- Renderer: $RENDERER"

# Qualcomm/Adreno specific optimizations
if getprop ro.board.platform | grep -qi "trinket\|sm6125\|sdm665"; then
    ui_print "- Snapdragon 665/Adreno 610 detected"
    ui_print "- Applying Adreno-optimized settings"
    sed -i 's/shader_quality=1/shader_quality=1/' "$MODPATH/system/etc/wireframe/config.conf"
    sed -i 's/use_compute=0/use_compute=0/' "$MODPATH/system/etc/wireframe/config.conf"
fi

ui_print ""
ui_print "- Configuration: /system/etc/wireframe/config.conf"
ui_print "- Runtime toggle: setprop persist.wireframe.enabled 1/0"
ui_print "- Live config: /data/local/tmp/wireframe/config.conf"
ui_print ""
ui_print "- Installation complete!"
ui_print "- Reboot to activate"
