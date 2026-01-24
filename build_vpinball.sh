#!/bin/bash
set -e

export ANDROID_NDK_HOME=/mnt/c/Users/johnn/AppData/Local/Android/Sdk/ndk/27.2.12479018
export BUILD_TYPE=Release

echo "NDK: $ANDROID_NDK_HOME"

cd /mnt/c/vpinball-master

# Clean and configure
rm -rf build/android-arm64-v8a
mkdir -p build/android-arm64-v8a

cmake \
    -DPLATFORM=android \
    -DARCH=arm64-v8a \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_ANDROID_NDK=$ANDROID_NDK_HOME \
    -B build/android-arm64-v8a

# Build with limited parallelism to avoid memory issues
cmake --build build/android-arm64-v8a -- -j4

echo "Build complete!"
