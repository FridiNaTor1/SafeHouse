# Sly 2 near-plane clipping regression

A small, game-data-free Vulkan test for `Sly2-Release/glob.geom`. It renders 27 triangles:
each input vertex is inside, exactly on, or outside the near plane. The vertices have different
clip-space W, colors, UVs and fog values, exercising affine color/fog interpolation and
perspective-correct UV interpolation. Output is a 320 x 320 RGBA8 image.

Requires a C++17 compiler, Vulkan headers/loader and a Vulkan device (Mesa lavapipe works),
plus `glslangValidator`. Run from this directory:

```sh
glslangValidator -V --auto-map-locations test.vert -o vert.spv
glslangValidator -V --auto-map-locations test.frag -o frag.spv
glslangValidator -V --auto-map-locations ../../Sly2-Release/glob.geom -o geom.spv
c++ -std=c++17 render.cpp -lvulkan -o clip-test
./clip-test vert.spv geom.spv frag.spv after.rgba
```

To compare a reference revision, export its `Sly2-Release/glob.geom` to `before.geom`, compile
it with the same command, run the same test to `before.rgba`, then `cmp before.rgba after.rgba`.
Use a device that can compile the original shader for this comparison. On Mali-G615 MC6 driver
r44p1 the original crashes during `vkCreateGraphicsPipelines`; the unified-emission shader compiles
and runs. For Android, cross-compile `render.cpp` using the NDK's API26+ ARM64 clang++, adding
`-static-libstdc++`, then run the binary and SPIR-V files through ADB in `/data/local/tmp`.
No window, game files, network permission or installed game modification is needed.

This test covers clipping/interpolation, not the complete game's rendering or performance.
