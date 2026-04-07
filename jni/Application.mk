APP_ABI := arm64-v8a armeabi-v7a
APP_PLATFORM := android-26
APP_STL := c++_static
APP_CPPFLAGS := -std=c++20 -O3 -ffast-math -fno-exceptions -fno-rtti \
                -ffunction-sections -fdata-sections -fvisibility=hidden \
                -DNDEBUG -flto=thin
APP_LDFLAGS := -Wl,--gc-sections -Wl,--strip-all -flto=thin -O3
