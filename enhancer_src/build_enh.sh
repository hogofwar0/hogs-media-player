#!/bin/sh
# Build the headless AI enhancer (hogs_enhance.exe): ncnn Real-ESRGAN + RIFE + FFmpeg H.265.
# Run from a w64devkit / MSYS shell:   sh enhancer_src/build_enh.sh
# Needs (see README.md "Building"):
#   W64DEVKIT   MinGW-w64 GCC toolchain folder            (default below)
#   HOGS_VULKAN_SDK  LunarG Vulkan SDK folder (Include + Lib) (default below; a separate name because a
#                    stale system-wide VULKAN_SDK variable is common)
#   ../deps/ncnn-install                ncnn built with Vulkan (include/ncnn + lib/*.a)
#   ../deps/ffmpeg-n7.1-win64-gpl-shared   BtbN FFmpeg-Builds n7.1 win64 gpl-shared
# OUT=../hogs_enhance_t.exe builds a test copy instead of replacing ../hogs_enhance.exe.
set -e
cd "$(dirname "$0")"
W64DEVKIT="${W64DEVKIT:-/c/AI Resources help from hogofwar/w64devkit}"
VK_SDK="${HOGS_VULKAN_SDK:-/c/AI Resources help from hogofwar/VulkanSDK/1.4.350.0}"
export PATH="$W64DEVKIT/bin:$PATH"
# local deps are referenced RELATIVELY (../deps/...): absolute paths containing an apostrophe broke
# gcc/ld once; the toolchain paths may contain spaces, which gcc handles fine.
NCNN_INC="../deps/ncnn-install/include/ncnn"        # vendored: headers self-sufficient (no ncnn/src)
NCNN_LIB="../deps/ncnn-install/lib"
VK_INC="$VK_SDK/Include"
VK_LIB="$VK_SDK/Lib"
FF="../deps/ffmpeg-n7.1-win64-gpl-shared"

# bridge + custom rife.Warp layer
g++ -O2 -c ncnn_bridge.cpp  -o ncnn_bridge.o  -I "$NCNN_INC" -I "$VK_INC"
g++ -O2 -c warp.cpp         -o warp.o         -I "$NCNN_INC" -I "$VK_INC"
# enhancer TU: ffmpeg headers + the opaque ncnn_bridge.h only (NO ncnn/vulkan headers)
g++ -O2 -c hogs_enhance.cpp -o hogs_enhance.o -I "$FF/include"

g++ hogs_enhance.o ncnn_bridge.o warp.o -o "${OUT:-../hogs_enhance.exe}" \
  -Wl,--start-group "$NCNN_LIB/libncnn.a" "$NCNN_LIB/libglslang.a" "$NCNN_LIB/libSPIRV.a" \
  "$NCNN_LIB/libMachineIndependent.a" "$NCNN_LIB/libGenericCodeGen.a" "$NCNN_LIB/libOSDependent.a" \
  "$NCNN_LIB/libglslang-default-resource-limits.a" -Wl,--end-group \
  -L "$VK_LIB" -lvulkan-1 \
  -L "$FF/lib" -lavformat -lavcodec -lavutil -lswscale -lavfilter \
  -fopenmp -lpthread -lpdh -ldxgi -ldxguid
echo "BUILT -> ${OUT:-../hogs_enhance.exe}"   # OUT=../hogs_enhance_t.exe to build a test copy
