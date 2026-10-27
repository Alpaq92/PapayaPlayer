# Phase 0 spike results

Run on 2026-10-26 on Windows 11 x64 (this machine): clang-cl 23.1.4, Rust 1.99.1
(MSVC), CMake 4.4.3, Visual Studio 2026 toolset. Other platforms in CI at
https://github.com/Alpaq92/PapayaPlayer: all 15 jobs green on `662d9eb`
(Windows, macOS, Linux shared + static, ASan/UBSan, TSan, lint, three BSDs),
2–5 minutes each with ccache.

| Spike | Result | Evidence / next step |
|---|---|---|
| **S1** libxaac on Windows | ✅ **Pass.** Builds unmodified with its own CMake and clang-cl (library + CLI) | `spikes/ittiam-windows.ps1 -Libs libxaac` |
| **S2** libavc, libhevc, libmpeg2 on Windows | ✅ **Pass with our Win32 threading layer.** Every decoder source, SIMD included, compiles with clang-cl; only `common/ithread.c` (pthreads) fails. `papaya/decode/sw/ittiam/ithread_win32.c` replaces it (no patch); all three decoders then link and their entry points resolve. Later replaced by `ithread_papaya.c`, one layer over papaya's threads for every OS | `spikes/ittiam-windows.ps1`, `spikes/ittiam-inject.cmake`. Their example programs need `sys/time.h` (not needed by us) |
| S2 finding | ⚠️ libavc's CMake compiles **every** file with `-mavx2 -mfma` on x86, so the library could crash on CPUs without AVX2. libhevc / libmpeg2 use `-msse4.2 -mno-avx` | Our own CMake targets (phase 2) set AVX2 flags only on the `*_avx2.c` files. Also: their `link_libraries(... m)` breaks Windows links |
| **S3** NV12 + P010 through Reaktor | ✅ **Pass on Direct3D 11:** NV12 (BT.709 limited) and P010 HDR10 textures draw through Nuklear's image path (`nk_image` handle = `SDL_Texture *`); SDL tone-maps HDR10 to the SDR output itself | `spikes/video_texture.c`, `-DPAPAYA_SPIKES=ON`, `--renderer direct3d11`. P010 needs a YCbCr colorspace from `SDL_DEFINE_COLORSPACE` (BT.2020 / PQ); `SDL_COLORSPACE_HDR10` is the RGB one |
| S3 finding | ⚠️ **HDR output** (not just tone-mapped SDR) needs `SDL_PROP_RENDERER_CREATE_OUTPUT_COLORSPACE_NUMBER` when the renderer is created, and Reaktor creates the renderer: **Reaktor needs an option for the output colorspace** | Reaktor change |
| S3 finding | Software renderer: NV12 works but shows a small corrupt block at the bottom-right; P010 unsupported (expected: no HDR on software) | Investigate the NV12 artefact before relying on the software fallback |
| **S4** custom shaders (SDL GPU render state, shadercross) | ⚠️ `SDL_CreateGPURenderState` needs a renderer with a GPU device, i.e. SDL's `gpu` renderer, and **Reaktor forces `SDL_GPU` off**. It cannot run on the native D3D11 / Metal renderers the zero-copy plan uses | Not needed for HDR: SDL's D3D11 renderer already converts YUV, outputs HDR10 and tone-maps (built-in operator, not BT.2390). Custom shaders (Lanczos, deinterlace, BT.2390) need either a Reaktor option for SDL's GPU renderer or our own pass. Decide before phase 9 |
| **S5** Reaktor CSS | ⚠️ Supports `border-radius` (per corner), borders, `background-color`, `opacity`, sizes, margins, padding, media queries. **No gradients, no `box-shadow`** | Pills and outlined rounded bars work in CSS. Gradients: draw with Nuklear's multi-colour rect (`nk_fill_rect_multi_color`) in our widgets, or add gradients to Reaktor's CSS |
| **S6** miniaudio passthrough | ❌ miniaudio handles PCM only (`u8`, `s16`, `s24`, `s32`, `f32`); no IEC 61937 / non-PCM output | Passthrough (phase 11) needs its own small WASAPI / CoreAudio / ALSA backend, as planned. BSD backends confirmed in its docs: OSS (FreeBSD), sndio (OpenBSD), audio(4) (NetBSD) |
| **S7** cros-codecs C ABI | ⚠️ cros-codecs is a Rust API with no C ABI | Our own small Rust crate exposing the parsers and VA-API decoder to C (Rust and Corrosion already work, see below). Build on BSD unverified |
| **S8** BSD baseline | ✅ **Pass.** SDL3, Reaktor, Rust (Corrosion), the core library and tests build and pass on FreeBSD 15.1, NetBSD 11.0 and OpenBSD 7.9, shared and static (CI run on commit `662d9eb`). Ittiam libraries not yet built on BSD (they join the build in phase 2) | `.github/workflows/bsd.yml` |
| Build skeleton | ✅ Shared (`papaya_core.dll`, Rust `cdylib`) and static (single exe) builds, unit tests (greatest + CTest), Rust crate through Corrosion, Reaktor app window, license check, clang-tidy clean | `./build.ps1 -Test`, `./build.ps1 -Out build-static -Test -DPAPAYA_SHARED_LIBS=OFF` |
