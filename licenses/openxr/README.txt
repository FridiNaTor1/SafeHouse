OpenXR headers and loader (Khronos Group), release 1.1.63
Source: https://github.com/KhronosGroup/OpenXR-SDK/releases/tag/release-1.1.63
  (archive https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.63.tar.gz,
   sha256 5b8dac0608f49498fe8e95f562fe6bcf0bc781e561f137348a8c251d06a88b6f)
Headers (Libraries/include/openxr: openxr.h, openxr_platform.h, openxr_platform_defines.h): Apache-2.0 OR MIT
  (SPDX-License-Identifier in each file; both licence texts here).
Loader (libopenxr_loader / openxr_loader.dll): Apache-2.0.
  Android: org.khronos.openxr:openxr_loader_for_android:1.1.63 from Maven Central
  (https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/1.1.63/, sha256 of the .aar
   622419d2f6741c3443a3beb4779af0764318edd01830de967f24c741ebcded73), packaged as lib/arm64-v8a/libopenxr_loader.so.
  Desktop (Linux x86_64 / aarch64, Windows): built from the same OpenXR-SDK release archive (cmake/openxr-loader.cmake,
  the loader target only, with the SDK's bundled jsoncpp: MIT, jsoncpp-LICENSE.txt here) and shipped beside the games as
  lib/libopenxr_loader.so.1 (Linux) or bin/openxr_loader.dll (Windows). Loaded at run time only when VR is asked for
  (--vr, the launcher's "Play in VR" and its headset check); the game starts and runs normally without it.
