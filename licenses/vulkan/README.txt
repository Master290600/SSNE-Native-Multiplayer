SSNE optional Vulkan runtime for Windows x64

Mesa 26.3.0 and LLVM 23.1.5 are contained in the mesa-dist-win 26.3.0 MSVC
release. The two unmodified files are x64/opengl32.dll and x64/libgallium_wgl.dll.
They implement the existing OpenGL renderer on the selected Vulkan device using
Mesa's Zink driver. Vulkan itself is supplied by the graphics driver.

Windows binary archive, build scripts and distribution notices:
https://github.com/pal1000/mesa-dist-win/releases/tag/26.3.0

Mesa source and per-file copyright notices:
https://gitlab.freedesktop.org/mesa/mesa/-/tree/mesa-26.3.0

LLVM source and notices:
https://github.com/llvm/llvm-project/tree/llvmorg-23.1.5
