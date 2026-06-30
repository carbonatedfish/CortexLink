# CMake toolchain file for RK3588 (ARM64 / aarch64) cross-compilation
#
# Usage:
#   cmake -B build_rk3588 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/rk3588_toolchain.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build_rk3588
#
# Prerequisites:
#   sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
#
# For sysroot with all target libraries (recommended):
#   export RK3588_SYSROOT=/path/to/rk3588/sysroot
#   Then re-run cmake (this file picks up RK3588_SYSROOT from environment).

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# --- Cross compiler ---
set(CMAKE_C_COMPILER    aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER  aarch64-linux-gnu-g++)

set(CMAKE_AR      aarch64-linux-gnu-ar      CACHE FILEPATH "Archiver")
set(CMAKE_LINKER  aarch64-linux-gnu-ld      CACHE FILEPATH "Linker")
set(CMAKE_NM      aarch64-linux-gnu-nm      CACHE FILEPATH "NM")
set(CMAKE_OBJCOPY aarch64-linux-gnu-objcopy CACHE FILEPATH "Objcopy")
set(CMAKE_OBJDUMP aarch64-linux-gnu-objdump CACHE FILEPATH "Objdump")
set(CMAKE_RANLIB  aarch64-linux-gnu-ranlib  CACHE FILEPATH "Ranlib")
set(CMAKE_STRIP   aarch64-linux-gnu-strip   CACHE FILEPATH "Strip")

# --- Sysroot (optional, set RK3588_SYSROOT env var to use) ---
if(DEFINED ENV{RK3588_SYSROOT})
    set(CMAKE_SYSROOT $ENV{RK3588_SYSROOT})
    set(CMAKE_FIND_ROOT_PATH ${CMAKE_SYSROOT})
    set(CMAKE_SKIP_RPATH TRUE)
endif()

# --- Find strategy: search sysroot first, then host ---
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# --- RK3588-specific CPU tuning ---
# RK3588: 4× Cortex-A76 (performance) + 4× Cortex-A55 (efficiency)
# Target Cortex-A76 for application code to get best performance.
# Use ARMv8.2-A since A76 fully implements it (with dotprod, fp16, i8mm).
set(CMAKE_C_FLAGS_INIT   "-march=armv8.2-a+crypto+dotprod+fp16 -mtune=cortex-a76")
set(CMAKE_CXX_FLAGS_INIT "-march=armv8.2-a+crypto+dotprod+fp16 -mtune=cortex-a76")

# --- PKG_CONFIG path for cross-compiled libraries ---
if(DEFINED ENV{PKG_CONFIG_PATH})
    set(ENV{PKG_CONFIG_PATH} "$ENV{RK3588_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:$ENV{RK3588_SYSROOT}/usr/share/pkgconfig:$ENV{PKG_CONFIG_PATH}")
else()
    if(DEFINED ENV{RK3588_SYSROOT})
        set(ENV{PKG_CONFIG_PATH} "$ENV{RK3588_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:$ENV{RK3588_SYSROOT}/usr/share/pkgconfig")
    endif()
endif()

# --- Optional: set pkg-config wrapper for cross-compilation ---
# If you have aarch64-linux-gnu-pkg-config installed, uncomment:
# set(PKG_CONFIG_EXECUTABLE aarch64-linux-gnu-pkg-config CACHE FILEPATH "pkg-config for aarch64")
