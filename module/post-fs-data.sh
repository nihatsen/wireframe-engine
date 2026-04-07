#!/system/bin/sh

MODDIR=${0%/*}

# Set default properties early
resetprop persist.wireframe.enabled 1
resetprop persist.wireframe.quality 1
resetprop persist.wireframe.scale 1.0

# Persistent config survives reboots; runtime copy is what the engine reads
CONFIG_DEFAULT="$MODDIR/system/etc/wireframe/config.conf"
CONFIG_PERSIST="/data/adb/wireframe/config.conf"
CONFIG_RUNTIME="/data/local/tmp/wireframe/config.conf"

mkdir -p /data/adb/wireframe
mkdir -p /data/local/tmp/wireframe

if [ -f "$CONFIG_PERSIST" ] && [ ! -f "$CONFIG_RUNTIME" ]; then
    # Runtime was wiped (e.g. /data/local/tmp cleared) — restore from persistent
    cp "$CONFIG_PERSIST" "$CONFIG_RUNTIME"
    chmod 0666 "$CONFIG_RUNTIME"
elif [ ! -f "$CONFIG_PERSIST" ] && [ -f "$CONFIG_RUNTIME" ]; then
    # Persistent missing — back up runtime to persistent
    cp "$CONFIG_RUNTIME" "$CONFIG_PERSIST"
    chmod 0666 "$CONFIG_PERSIST"
elif [ ! -f "$CONFIG_PERSIST" ] && [ ! -f "$CONFIG_RUNTIME" ]; then
    # First install — seed both from defaults
    cp "$CONFIG_DEFAULT" "$CONFIG_PERSIST"
    cp "$CONFIG_DEFAULT" "$CONFIG_RUNTIME"
    chmod 0666 "$CONFIG_PERSIST" "$CONFIG_RUNTIME"
fi
# If both exist, leave them alone — runtime has the live user config

# Adreno GPU performance hints
if [ -f /sys/class/kgsl/kgsl-3d0/devfreq/governor ]; then
    echo "performance" > /sys/class/kgsl/kgsl-3d0/devfreq/governor 2>/dev/null
fi

# Reduce GPU idle timeout to keep GPU warm for our shader passes
if [ -f /sys/class/kgsl/kgsl-3d0/idle_timer ]; then
    echo 80 > /sys/class/kgsl/kgsl-3d0/idle_timer 2>/dev/null
fi
