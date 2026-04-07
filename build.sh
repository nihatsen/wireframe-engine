#!/bin/bash
set -e

# ═══════════════════════════════════════════════
#  Build script for Wireframe Render Engine
# ═══════════════════════════════════════════════

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
JNI_DIR="$SCRIPT_DIR/jni"
MODULE_DIR="$SCRIPT_DIR/module"
BUILD_DIR="$SCRIPT_DIR/build"
OUT_DIR="$SCRIPT_DIR/out"

# Check for NDK
if [ -z "$ANDROID_NDK_HOME" ]; then
    if [ -z "$ANDROID_NDK" ]; then
        # Try common locations
        for dir in \
            "$HOME/Android/Sdk/ndk/"* \
            "/opt/android-ndk"* \
            "$HOME/android-ndk"*; do
            if [ -d "$dir" ]; then
                export ANDROID_NDK_HOME="$dir"
                break
            fi
        done
    else
        export ANDROID_NDK_HOME="$ANDROID_NDK"
    fi
fi

if [ -z "$ANDROID_NDK_HOME" ] || [ ! -d "$ANDROID_NDK_HOME" ]; then
    echo "ERROR: Android NDK not found!"
    echo "Set ANDROID_NDK_HOME environment variable"
    echo ""
    echo "Install NDK:"
    echo "  sdkmanager --install 'ndk;26.1.10909125'"
    exit 1
fi

echo "═══════════════════════════════════════════════"
echo " Building Wireframe Render Engine"
echo " NDK: $ANDROID_NDK_HOME"
echo "═══════════════════════════════════════════════"

# Check for zygisk.hpp
if [ ! -f "$JNI_DIR/zygisk.hpp" ]; then
    echo "Downloading zygisk.hpp..."
    curl -sL "https://raw.githubusercontent.com/topjohnwu/Magisk/master/native/src/external/include/zygisk.hpp" \
        -o "$JNI_DIR/zygisk.hpp"

    if [ ! -s "$JNI_DIR/zygisk.hpp" ]; then
        echo "ERROR: Failed to download zygisk.hpp"
        echo "Manually download from:"
        echo "  https://github.com/topjohnwu/Magisk/blob/master/native/src/external/include/zygisk.hpp"
        exit 1
    fi
fi

# Clean previous build
rm -rf "$BUILD_DIR" "$OUT_DIR"
mkdir -p "$BUILD_DIR" "$OUT_DIR"

# Build native libraries
echo ""
echo "── Compiling native code ──"
cd "$JNI_DIR"
"$ANDROID_NDK_HOME/build/ndk-build" \
    NDK_PROJECT_PATH="$BUILD_DIR" \
    NDK_APPLICATION_MK="$JNI_DIR/Application.mk" \
    APP_BUILD_SCRIPT="$JNI_DIR/Android.mk" \
    NDK_OUT="$BUILD_DIR/obj" \
    NDK_LIBS_OUT="$BUILD_DIR/libs" \
    -j$(nproc) \
    V=0

echo ""
echo "── Build successful ──"

# Copy built libraries to module
echo "── Packaging module ──"
mkdir -p "$MODULE_DIR/zygisk"

# arm64-v8a
if [ -f "$BUILD_DIR/libs/arm64-v8a/libwireframe_engine.so" ]; then
    cp "$BUILD_DIR/libs/arm64-v8a/libwireframe_engine.so" \
       "$MODULE_DIR/zygisk/arm64-v8a.so"
    chmod 755 "$MODULE_DIR/zygisk/arm64-v8a.so"
    echo "  ✓ arm64-v8a: $(du -h "$MODULE_DIR/zygisk/arm64-v8a.so" | cut -f1)"
fi

# armeabi-v7a
if [ -f "$BUILD_DIR/libs/armeabi-v7a/libwireframe_engine.so" ]; then
    cp "$BUILD_DIR/libs/armeabi-v7a/libwireframe_engine.so" \
       "$MODULE_DIR/zygisk/armeabi-v7a.so"
    chmod 755 "$MODULE_DIR/zygisk/armeabi-v7a.so"
    echo "  ✓ armeabi-v7a: $(du -h "$MODULE_DIR/zygisk/armeabi-v7a.so" | cut -f1)"
fi

echo ""
echo "── Creating flashable ZIP ──"
cd "$MODULE_DIR"
ZIP_NAME="wireframe-engine-v1.0.0-$(date +%Y%m%d).zip"
zip -r9 "$OUT_DIR/$ZIP_NAME" . \
    -x "*.DS_Store" \
    -x "*__MACOSX*"

echo ""
echo "═══════════════════════════════════════════════"
echo " BUILD COMPLETE"
echo " Output: out/$ZIP_NAME"
echo " Size: $(du -h "$OUT_DIR/$ZIP_NAME" | cut -f1)"
echo "═══════════════════════════════════════════════"
