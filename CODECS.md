# PapayaPlayer: codecs, containers and subtitles

Permissive options we can bundle, and the gaps that remain. Compiled 2026-10-26;
evidence and file paths are in [BACKEND-RESEARCH.md](BACKEND-RESEARCH.md)
(sections 4, 5, 5b and 10).

**Order of use for every format:** OS decoder (hardware) → bundled permissive
library. **No FFmpeg for the time being** (decision 2026-10-26), not even loaded at
run time. Formats in section 5 therefore play only where the OS can decode them,
or through passthrough, and otherwise are reported as unsupported.

**Legend**
- **Port**: source in another language (Java, Rust) that we translate to C.
- **Rust**: a Rust crate, linked as a static library through a C ABI, or ported to C.
- ⚠️ **Check provenance**: new, AI-assisted code that claims to be written from
  the spec. Spot-check it against FFmpeg's code before copying, and test it with
  our own sample files. No candidate marked this way has passed an official
  conformance suite.
- **(unverified)**: not checked in source or in a primary document.

---

## 1. Video decoders

| Codec | Library | License | Language | Notes |
|---|---|---|---|---|
| H.264 | **libavc** (Ittiam / Android) | Apache-2.0 | C | Interlaced (MBAFF) code present. No Windows build listed |
| HEVC | **libhevc** (Ittiam / Android) | Apache-2.0 | C | Main, Main 10 (`u4_enable_hbd`), RExt up to 4:2:2. No Windows build listed |
| HEVC (backup) | **hevc.js** (lid-labs) | MIT | C++17 | Main + Main 10, all 147 v1 conformance streams bit-exact. GCC/clang only, ignores SEI/VUI colour info |
| AV1 | **dav1d** | BSD-2 | C + asm | Meson + NASM build |
| VP8 / VP9 | **libvpx** | BSD-3 | C + asm | Own configure script |
| MPEG-2 | **libmpeg2** (Ittiam / Android; not the GPL library of the same name) | Apache-2.0 | C | No Windows build listed |
| MPEG-4 Part 2 / H.263 (DivX, Xvid) | Android **m4v_h263** | Apache-2.0 | C | `frameworks/av/media/module/codecs/m4v_h263/dec` |
| MPEG-1 | **pl_mpeg** | MIT | C | Unless libmpeg2 also handles MPEG-1 (unverified) |
| ProRes (all 6 profiles, 10/12-bit, alpha, interlaced) | **oxideav-prores** | MIT | Rust | ⚠️ Check provenance |
| MJPEG | **libjpeg-turbo** | BSD / IJG | C | Optional |
| Theora | **libtheora** | BSD | C | Optional |
| VVC / H.266 | **vvdec** (Fraunhofer) | BSD-3-Clear | C++ | Later |
| DNxHD / DNxHR | **filmcraft-dnx** (candidate) | MIT OR Apache-2.0 | Rust | Says clean-room from SMPTE ST 2019-1. **No public repository** listed on crates.io, so provenance can't be checked yet |
| APV | **libopenapv** (Android) | BSD-3 | C | Optional |

**VC-1 hardware path:** no permissive software decoder exists (see gaps), but
permissive VC-1 header parsers can feed hardware decoders (VA-API, D3D11VA, NVDEC):
- **Intel vpl-gpu-rt** UMC `vc1_dec` / `vc1_common`: MIT, C++, maintained. Needs its UMC/ippcore support layer stripped out.
- **Intel libyami** `vc1Parser.cpp`: Apache-2.0, archived. Simpler, so a useful second reference.

---

## 2. Audio decoders

| Codec | Library | License | Language | Notes |
|---|---|---|---|---|
| AAC (LC, HE v1/v2, xHE/USAC, LD/ELD) | **libxaac** (Ittiam / Android) | Apache-2.0 | C | Visual Studio build support. Replaces FDK-AAC |
| MP3 / MP2 / MP1 | **minimp3** or **dr_mp3** | CC0 / PD | C | MP1/MP2 support unverified |
| Opus | **libopus** | BSD-3 | C | |
| Vorbis | **libvorbis** + **libogg** | BSD | C | |
| FLAC | **libFLAC** or **dr_flac** | BSD / PD | C | |
| PCM / WAV | **dr_wav** | PD | C | |
| ALAC | **Apple ALAC** | Apache-2.0 | C++ | |
| WavPack | **libwavpack** | BSD | C | Optional |
| AMR-NB / AMR-WB | Android **amrnb / amrwb** | Apache-2.0 | C | Optional |
| G.711, GSM 06.10 | Android **g711**, **libgsm** | Apache-2.0 / TU Berlin (permissive) | C | Optional |
| AC-3 | **pschatzmann/codec-ac3** or **PasyDev/ac3psy** | MIT | C++ / C17 | Patents expired 2017. ⚠️ Check provenance |
| E-AC-3 (+ Atmos JOC) | **OpenJOC** (chyinan) | Apache-2.0 | Rust, has a C ABI | Last patent reported expired 2026-01-30 (unverified). ⚠️ Check provenance |
| DTS core | **oxideav-dts** | MIT | Rust | Core only: no XCH / X96 / XLL. ⚠️ Check provenance |
| TrueHD / MLP | **truehdd** (`truehd` crate) | Apache-2.0 | Rust | Its README says "experimental, not for production" |
| IAMF | **iamf_tools** (Android) | BSD-3-Clear | C++ | Optional |

**Audio passthrough:** AC-3, E-AC-3, DTS (including DTS-HD MA) and TrueHD can
also go **undecoded** over HDMI or S/PDIF to an AV receiver. This needs our own
IEC 61937 packing; no library is required.

---

## 3. Containers (demuxers)

| Format | Source | License | Notes |
|---|---|---|---|
| MP4 / MOV | **Port** Media3 `mp4/` (`Mp4Extractor`, `FragmentedMp4Extractor`, `BoxParser`; ~330 KB of Java, roughly 6–8k lines of C) | Apache-2.0 | Decided 2026-10-26. Normal + fragmented MP4, edit lists, `ctts`, keyframes, AV1, Opus, AC-3/E-AC-3, HDR boxes. Not chosen: **L-SMASH** (C, ISC; has `ctts`/`stss`/`elst`/`moof`/`co64`/HDR boxes, but **no AV1, VP9, Opus or FLAC sample entries** and last real commit 2020-03); **minimp4** (no `ctts`/`stss`/`elst`, no fragmented MP4, no AV1/Opus, 32-bit timestamps); Android `MPEG4Extractor` (tied to libstagefright) |
| MKV / WebM | **libwebm** (mkvparser) | BSD-3 | C++. Subtitles, chapters, seeking without Cues. nestegg (ISC, C) is the lighter alternative |
| MPEG-TS / M2TS / PS | Android **ATSParser** + **MPEG2PS/TSExtractor** | Apache-2.0 | Same libstagefright check; otherwise write our own |
| Ogg | **libogg** | BSD | |
| AVI | **Port** Media3 `avi/` (~1.3k lines) | Apache-2.0 | Add OpenDML (files over 1 GB) from the spec |
| FLV | **Port** Media3 `flv/` (~950 lines) | Apache-2.0 | H.264 + AAC/MP3/G.711 |
| ADTS, MP3, FLAC, WAV, AMR | Android extractors or our own | Apache-2.0 | Simple |

Media3 paths are under `libraries/extractor/src/main/java/androidx/media3/extractor/`
in https://github.com/androidx/media.

---

## 3b. Discs (decided 2026-10-26)

| Disc | Library | License | Notes |
|---|---|---|---|
| **DVD-Video** | **oxideav-dvd** | MIT | Rust, ~34k lines, active. ISO 9660 + UDF mount, IFO parser, VOB demux, navigation VM, SPU menus, angles. Says clean-room from ECMA-267/268 + mpucoder docs. Full menus for unencrypted discs, ISOs and decrypted rips. ⚠️ Check provenance |
| **Blu-ray (BDMV)** | **oxideav-bluray** | MIT | Rust, ~23k lines, active. UDF mount, index / MovieObject / MPLS / CLPI, m2ts streaming, multi-angle, PGS, HDMV navigation VM. ⚠️ Check provenance |
| Blu-ray menu layer | Our own, using **libhdmv** (Apache-2.0) IGS decoder + scene engine as code or reference | Apache-2.0 | oxideav-bluray leaves Interactive Graphics (buttons, pages, pop-ups) to the player as `NavRequest`s |

**oxideav-bluray gaps** (from its README "Deferred" list):
- **Interactive Graphics menus** and the button-state machine: ours to build (above).
- **BD-J** (Java menus): not supported, and no permissive implementation exists.
- Resume-intention handling and user-operation-mask *enforcement* (the mask decodes, nothing applies it yet).
- SubPath secondary-stream playback: picture-in-picture, out-of-mux audio, text subtitles.
- Raw drive mount (`/dev/sr0`, drive letters): the UDF mounter exists, routing not yet.
- Some VM arithmetic edge cases (`Div`/`Mod` overflow) defined only in the members-only BD-ROM book.
- Rare UDF layouts (ICB strategy types other than 4, multi-extent partition maps).
- **No AACS / BD+**: decrypted folders and ISOs only.

**oxideav-dvd gaps:** no CSS decryption (unencrypted / decrypted only).

## 4. Subtitles (parsing; Reaktor does the rendering)

| Format | Source | License | Notes |
|---|---|---|---|
| SRT | **Port** Media3 `text/subrip/SubripParser` (311 lines) | Apache-2.0 | |
| WebVTT | **Port** Media3 `text/webvtt/` (~2.2k lines) | Apache-2.0 | mozilla/vtt.js (Apache-2.0) as a second reference |
| ASS / SSA | **Port** Media3 `text/ssa/` | Apache-2.0 | Basic styling. Full styling would need libass (ISC), which needs FriBidi (LGPL) |
| PGS (Blu-ray) | **Port** Media3 `text/pgs/PgsParser` (276 lines) | Apache-2.0 | Add window/composition handling, with libpgs / pgs-rs (MIT) as a guide |
| VobSub in MKV | **Port** Media3 `text/vobsub/VobsubParser` (479 lines) | Apache-2.0 | |
| VobSub .idx/.sub files | Modelled on emk's `vobsub` crate | Apache-2.0 | Unmaintained since 2024 |
| DVB subtitles | **Port** Media3 `text/dvb/DvbParser` | Apache-2.0 | |
| CEA-608 / 708 captions | **Port** Media3 `text/cea/` | Apache-2.0 | |
| TTML, 3GPP timed text | Media3 `text/ttml/`, `text/tx3g/` | Apache-2.0 | Optional |

---

## 5. Gaps: no clean permissive option

| Format | Fallback |
|---|---|
| **VC-1 / WMV9** (software decode) | Media Foundation on Windows; hardware decode fed by Intel's parser elsewhere. Or write our own from SMPTE 421M |
| **WMV7 / WMV8**, WMV9 Screen (MSS1/MSS2) | Media Foundation on Windows; unsupported elsewhere |
| **WMA** (Standard, Pro, Lossless, Voice) | Media Foundation on Windows; unsupported elsewhere |
| **DTS-HD MA** (lossless part) | Play the DTS core, which every DTS-HD stream contains; passthrough for the lossless stream |
| **ASF** container | Write our own from Microsoft's spec, with jchv/asf-rs (ISC) as a reference. **Legal check needed on the spec's license**. Until then Media Foundation on Windows, unsupported elsewhere |
| ProRes RAW | VideoToolbox on macOS; unsupported elsewhere |
| HEVC 4:4:4 | Hardware decoders where available |
| AC-4 | **Candidate:** oxideav-ac4 (Rust, MIT, 2026-09): its README says it decodes channel-based, immersive and A-JOC object streams to PCM. ⚠️ Check provenance. Until then OS decoders / passthrough |
| MPEG-H 3D Audio | Fraunhofer mpeghdec and its Rust port `mpeghdec` use the FDK-style MPEG-H license (non-standard, like FDK-AAC): excluded. OS decoders where available |
| Encrypted DVDs (CSS) | None: no permissive library exists (`oxideav-css` is referenced but doesn't exist), and decrypting is legally restricted. Unencrypted discs, ISOs and decrypted rips play (section 3b) |
| Encrypted Blu-rays (AACS, BD+) | None: legally restricted (`oxideav-aacs` not evaluated). Decrypted folders and ISOs play (section 3b) |
| Blu-ray BD-J (Java) menus | None: no permissive BD-J exists. Fallback: play the main title / title list without menus |
| Blu-ray Interactive Graphics menus | Until our IG layer is done (section 3b): title list without menus |

---

## 6. Rejected

| Option | Reason |
|---|---|
| FDK-AAC | Use is conditional on holding AAC patent licenses; not OSI-approved; non-free in Debian/Fedora |
| OpenH264 | No interlaced; patent coverage only for Cisco's downloaded binary |
| minih264 | Encoder only |
| minimp4 | Missing `ctts`, `stss`, `elst`, fragmented MP4, AV1, Opus; 32-bit timestamps |
| L-SMASH (as MP4 demuxer) | No AV1, VP9, Opus or FLAC sample entries; unmaintained since 2020 |
| libde265, dcadec, liba52, libdca | LGPL / GPL |
| harletty `dca` / `eac3`, LibDTSX, remade_ffmpeg_rs, wmvkit, wmv-decoder crate, jcodec ProRes | Ported from FFmpeg (LGPL despite their labels) |
| oxideav-wma | Tables extracted from a Microsoft DLL |
| go-vc1 | I-frames only; comments say it mirrors FFmpeg |
| oxideav-msmpeg4 (MS-MPEG4 v1–v3 only, not WMV7/8/9) | VLC tables extracted from a Microsoft DLL, like oxideav-wma |
| oxideav-vfw (x86 emulator running Windows codec DLLs) | Needs Microsoft's DLLs supplied by the user; not a decoder we can ship |
| wmv-decoder crate (WMV1/2/3, progressive WVC1) | MPL-2.0 and states it follows FFmpeg's vc1.c |
| NihAV (Rust) | AGPL-3.0 |
| mpeghdec (C and Rust port) | Fraunhofer FDK-style license, same problems as FDK-AAC |
| rusty_h265, rust_h265 (Rust HEVC) | Main / Main 10 only, so they don't close HEVC 4:4:4; libhevc and hevc.js already cover Main 10 |
| turbores (ProRes) | MPL-2.0 and cites FFmpeg's proresdec.c |
| NihAV | AGPL-3.0 |

---

## 7. Open decisions

1. **Rust:** decided 2026-10-26: link the Rust crates as static libraries (see TECH-STACK.md).
2. **Android TS demuxer vs our own:** check how much of libstagefright ATSParser pulls in (MP4 is decided: Media3 port).
   MPEG4Extractor and ATSParser pull in.
3. **Windows builds** of libavc, libhevc, libmpeg2 and libxaac (start with libxaac).
4. **Provenance spot-checks** for every ⚠️ item before copying code.
5. **ASF spec legal check.**
6. **FFmpeg:** not used for now. Revisit only if the gaps in section 5 matter
   more than keeping the player FFmpeg-free.
7. **Patents:** HE-AAC/xHE-AAC, HEVC and VC-1 pools are still active (unverified for
   each). Ask a lawyer before any commercial release.
