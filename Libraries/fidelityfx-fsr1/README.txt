AMD FidelityFX Super Resolution 1.0 (FSR 1): the shader headers ffx_a.h and ffx_fsr1.h, unmodified, from
https://github.com/GPUOpen-Effects/FidelityFX-FSR (ffx-fsr/, commit a21ffb8f6c13233ba336352bdff293894c706575).
Copyright (c) 2021 Advanced Micro Devices, Inc. MIT licence: licenses/fidelityfx-fsr.txt.

They are not compiled. Sly2-Release/fsreasu.frag and fsrrcas.frag are GLSL fragment-program versions of their 32-bit
EASU and RCAS paths (FsrEasuF, FsrRcasF with FsrEasuCon / FsrRcasCon's constants), written so every renderer layer of
the games (OpenGL, OpenGL ES, the Vulkan and deko3d layers' offline compilers) builds them: texelFetch in place of the
gathers, no #include, the approximate reciprocals of ffx_a.h (APrxLoRcpF1, APrxLoRsqF1, APrxMedRcpF1) written out.
sha256:
f60e2722fcd13989523b9164d776ab382b3692791767f3bf8bb19967f763f3fb  ffx_a.h
93c3922362ea7fc99cbcc698ca30c98de4f8c246d1fbb0b09e015ddef38ce3a5  ffx_fsr1.h
