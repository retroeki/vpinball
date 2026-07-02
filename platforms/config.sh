#!/bin/bash

set -e

SDL_SHA=7f3ae3d57459e59943a4ecfefc8f6277ec6bf540
SDL_IMAGE_SHA=11154afb7855293159588b245b446a4ef09e574f
SDL_TTF_SHA=a1ce3670aec736ecbf0936c43f2f0cc53aa61e5b
FREEIMAGE_SHA=b1613452a0c3849d43ac877b154cf51ff9e078d3
BGFX_CMAKE_VERSION=1.135.9046-500
BGFX_PATCH_SHA=a20c34bbe621cde25c0b5826d90ffec6b9f499d9
PINMAME_SHA=6ba56f6a8d6b5c912441e4b6b9de2cfa670591cc
OPENXR_SHA=b15ef6ce120dad1c7d3ff57039e73ba1a9f17102
# Bumped from 5c916d53 to pull in libvni (unencrypted PIN2DMD .pal/.vni colorization).
# This is the SHA vpinball master pins, i.e. the version the bundled vni plugin
# is built against. Moves libserum/libzedmd/libpupdmd too — re-test Serum + ext DMD.
LIBDMDUTIL_SHA=5879c321e75c2ca3c5dd9cde5d7c49f0075d1f16
LIBALTSOUND_SHA=0656fc2eb39a6f4fdd557043c28cd8dfdc7e762f
LIBDOF_SHA=2711a23f7ec1085448f944145e0d63b7ab792033
FFMPEG_SHA=db69d06eeeab4f46da15030a80d539efb4503ca8
LIBZIP_SHA=6f8a0cdd24a0dc6cce9dac4a7679da784ab124ea

if [ -z "${BUILD_TYPE}" ]; then
   BUILD_TYPE="Release"
fi

echo "Build type: ${BUILD_TYPE}"
echo ""
