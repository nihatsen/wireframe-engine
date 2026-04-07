#!/system/bin/sh

MODDIR=${0%/*}

# Wait for boot completion
while [ "$(getprop sys.boot_completed)" != "1" ]; do
    sleep 1
done
sleep 3

LOG="/data/local/tmp/wireframe/engine.log"

log() {
    echo "$(date '+%H:%M:%S') [WireframeService] $1" >> "$LOG"
}

log "Service started"
log "Device: $(getprop ro.product.model)"
log "Android: $(getprop ro.build.version.release)"
log "SDK: $(getprop ro.build.version.sdk)"
log "Kernel: $(uname -r)"
log "GPU: $(getprop ro.hardware.egl)"

# Monitor for live config changes — persist and broadcast
inotifywait -m -e modify /data/local/tmp/wireframe/config.conf 2>/dev/null | while read event; do
    log "Config changed, broadcasting update"
    cp /data/local/tmp/wireframe/config.conf /data/adb/wireframe/config.conf 2>/dev/null
    setprop wireframe.config.version $(date +%s)
done &

# Performance monitor (optional - logs GPU load)
while true; do
    if [ "$(getprop persist.wireframe.debug)" = "1" ]; then
        GPU_BUSY=$(cat /sys/class/kgsl/kgsl-3d0/gpubusy 2>/dev/null)
        GPU_FREQ=$(cat /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null)
        log "GPU: busy=$GPU_BUSY freq=$GPU_FREQ"
    fi
    sleep 10
done &
