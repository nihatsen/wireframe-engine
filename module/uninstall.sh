#!/system/bin/sh

# Cleanup runtime and persistent files
rm -rf /data/local/tmp/wireframe
rm -rf /data/adb/wireframe

# Reset properties
resetprop --delete persist.wireframe.enabled
resetprop --delete persist.wireframe.quality
resetprop --delete persist.wireframe.scale
resetprop --delete persist.wireframe.debug

# Restore GPU governor
if [ -f /sys/class/kgsl/kgsl-3d0/devfreq/governor ]; then
    echo "msm-adreno-tz" > /sys/class/kgsl/kgsl-3d0/devfreq/governor 2>/dev/null
fi
