# Test data

| Files | Source | License |
|---|---|---|
| `av1/00000527.ivf` (8-bit), `av1/00000671.ivf` (10-bit) | [dav1d-test-data](https://code.videolan.org/videolan/dav1d-test-data) `8-bit/data` and `10-bit/data`; reference MD5s from the `meson.build` beside them | BSD-2-Clause, Â© VideoLAN and dav1d authors |
| `vpx/vp80-00-comprehensive-001.ivf`, `vpx/vp90-2-02-size-16x16.webm`, `vpx/vp92-2-20-10bit-yuv420.webm`, `vpx/vp91-2-04-yuv422.webm`, `vpx/vp91-2-04-yuv444.webm`, `vpx/vp93-2-20-10bit-yuv422.webm`, `vpx/vp93-2-20-10bit-yuv444.webm` and their per-frame `.md5` lists | [libvpx test vectors](https://storage.googleapis.com/downloads.webmproject.org/test_data/libvpx/) | BSD-3-Clause, © The WebM Project authors (libvpx's license) |
| `h264/BA1_Sony_D.264` (renamed from `.jsv`), `h264/CABA3_TOSHIBA_E.264`, `h264/CAPAMA3_Sand_F.264` | [ITU-T H.264.1 conformance suite](https://www.itu.int/wftp3/av-arch/jvt-site/draft_conformance/AVCv1/); MD5s are of the reference `.yuv` in each zip | ITU-T conformance bitstreams, published openly for decoder testing (not under an open-source license; FFmpeg's FATE redistributes the same files) |
| `hevc/AMP_A_Samsung_7.bin`, `hevc/PICSIZE_A_Bossen_1.bin`, `hevc/WP_A_MAIN10_Toshiba_3.bit` (Main 10) | [ITU-T H.265.1 conformance suite](https://www.itu.int/wftp3/av-arch/jctvc-site/bitstream_exchange/draft_conformance/HEVC_v1/); MD5s from the `.md5` in each zip | as above |
| `hevc/GENERAL_8b_444_RExt_Sony_2.bit`, `hevc/GENERAL_10b_422_RExt_Sony_1.bit`, `hevc/GENERAL_10b_444_RExt_Sony_2.bit` (RExt) | [H.265.1 RExt conformance](https://www.itu.int/wftp3/av-arch/jctvc-site/bitstream_exchange/draft_conformance/RExt/); MD5s of the `_rec.yuv` from the `.md5` in each zip | as above |

WebM samples come from libwebm's own `testing/testdata` (submodule, BSD-3).
