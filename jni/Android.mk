LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := wireframe_engine

LOCAL_SRC_FILES := \
    main.cpp \
    hook.cpp \
    renderer.cpp \
    config.cpp \
    video_hook.cpp

LOCAL_C_INCLUDES := $(LOCAL_PATH)

LOCAL_LDLIBS := -llog -lEGL -lGLESv3 -lGLESv2 -landroid -ldl

LOCAL_CPPFLAGS := -std=c++20 -O3 -ffast-math -Wall -Wextra \
                  -fvisibility=hidden -DNDEBUG \
                  -fno-exceptions -fno-rtti \
                  -ftree-vectorize

# -mfpu=neon only for ARM32
ifeq ($(TARGET_ARCH_ABI),armeabi-v7a)
    LOCAL_CPPFLAGS += -mfpu=neon
endif

# Adreno-specific: prefer mediump in shaders, align memory
LOCAL_CPPFLAGS += -DADRENO_OPTIMIZE=1

LOCAL_LDFLAGS := -Wl,--gc-sections -Wl,--strip-all

include $(BUILD_SHARED_LIBRARY)
