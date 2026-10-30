/* HEVC through libhevc (Apache-2.0, Android's software decoder). */
#include "decode_internal.h"
#include "ihevc_typedefs.h"
#include "iv.h"
#include "ivd.h"
#include "ihevcd_cxa.h"

#define ITT_PREFIX(x) ihevcd_cxa_##x
#define ITT_API ihevcd_cxa_api_function
#define ITT_CORES_CMD IHEVCD_CXA_CMD_CTL_SET_NUM_CORES
/* Main 10: 16-bit output samples. RExt: every chroma format (one bit per
 * chroma_format_idc, 4:0:0 to 4:4:4). */
#define ITT_CREATE_EXTRA(ip) ((ip).u4_enable_hbd = 1, (ip).u4_enable_yuv_formats = 0xf)
#define ITT_BIT_DEPTH(op) ((op)->u4_bit_depth)

/* HDR in a raw stream (MPEG-TS, Annex B) is known only from the bitstream:
 * the SPS's VUI and the mastering display SEI. */
static void hevc_stream_color(void *ctx);
static void hevc_picture_color(void *ctx, papaya_frame *f);
#define ITT_STREAM_COLOR(c) hevc_stream_color(c)
#define ITT_PICTURE_COLOR(c, f) hevc_picture_color(c, f)
/* RExt 4:2:2 / 4:4:4: libhevc outputs those only when created for them. */
static int hevc_sps_chroma(const uint8_t *p, size_t n);
#define ITT_CHROMA_FORMAT(p, n) hevc_sps_chroma(p, n)

#include "ittiam_video.h"

/* hvcC (ISO/IEC 14496-15 8.3.3.1): arrays of VPS/SPS/PPS/SEI units and the
 * NAL length size. Anything else is taken as Annex B. */
static int
itt_to_annexb_config(itt_ctx *c, const uint8_t *p, size_t n)
{
    size_t at = 23;
    int arrays;

    if (n < 23 || p[0] != 1)
        return 0;
    c->length_size = (p[21] & 3) + 1;
    arrays = p[22];
    for (int a = 0; a < arrays && at + 3 <= n; a++) {
        int count = p[at + 1] << 8 | p[at + 2];

        at += 3;
        for (int i = 0; i < count && at + 2 <= n; i++) {
            size_t len = (size_t)p[at] << 8 | p[at + 1];

            at += 2;
            if (len > n - at)
                return -1;
            if (itt_config_nal(c, p + at, len) != 0)
                return -1;
            at += len;
        }
    }
    return 0;
}

/* The VUI's colour description, for whatever the container left
 * unspecified (2). */
static void
hevc_stream_color(void *ctx)
{
    itt_ctx *c = ctx;
    ihevcd_cxa_ctl_get_vui_params_ip_t ip;
    ihevcd_cxa_ctl_get_vui_params_op_t op;

    memset(&ip, 0, sizeof ip);
    memset(&op, 0, sizeof op);
    ip.e_cmd = IVD_CMD_VIDEO_CTL;
    ip.e_sub_cmd = (IVD_CONTROL_API_COMMAND_TYPE_T)IHEVCD_CXA_CMD_CTL_GET_VUI_PARAMS;
    ip.u4_size = sizeof ip;
    op.u4_size = sizeof op;
    if (ITT_API(c->codec, &ip, &op) != IV_SUCCESS || !op.u1_video_signal_type_present_flag)
        return;
    if (op.u1_colour_description_present_flag) {
        if (c->color.primaries == 2)
            c->color.primaries = op.u1_colour_primaries;
        if (c->color.transfer == 2)
            c->color.transfer = op.u1_transfer_characteristics;
        if (c->color.matrix == 2)
            c->color.matrix = op.u1_matrix_coefficients;
    }
    if (!c->color.full_range)
        c->color.full_range = op.u1_video_full_range_flag;
}

/* The mastering display SEI of the picture on display, when the container
 * gave no luminance (units of 0.0001 cd/m2). */
static void
hevc_picture_color(void *ctx, papaya_frame *f)
{
    itt_ctx *c = ctx;
    ihevcd_cxa_ctl_get_sei_mastering_params_ip_t ip;
    ihevcd_cxa_ctl_get_sei_mastering_params_op_t op;

    if (f->color.max_luminance > 0)
        return;
    memset(&ip, 0, sizeof ip);
    memset(&op, 0, sizeof op);
    ip.e_cmd = IVD_CMD_VIDEO_CTL;
    ip.e_sub_cmd = (IVD_CONTROL_API_COMMAND_TYPE_T)IHEVCD_CXA_CMD_CTL_GET_SEI_MASTERING_PARAMS;
    ip.u4_size = sizeof ip;
    op.u4_size = sizeof op;
    if (ITT_API(c->codec, &ip, &op) != IV_SUCCESS)
        return;
    f->color.max_luminance = (float)op.u4_max_display_mastering_luminance / 10000.0f;
    f->color.min_luminance = (float)op.u4_min_display_mastering_luminance / 10000.0f;
}

/* Bits of one NAL unit, skipping emulation prevention bytes. */
typedef struct {
    const uint8_t *p;
    size_t n, at;
    int bit, zeros;
    uint8_t cur;
} hevc_bits;

static int
hevc_bit(hevc_bits *b)
{
    if (b->bit == 0) {
        if (b->at >= b->n)
            return -1;
        b->cur = b->p[b->at++];
        if (b->zeros >= 2 && b->cur == 3) { /* 00 00 03: drop the 03 */
            b->zeros = 0;
            if (b->at >= b->n)
                return -1;
            b->cur = b->p[b->at++];
        }
        b->zeros = b->cur == 0 ? b->zeros + 1 : 0;
        b->bit = 8;
    }
    return b->cur >> --b->bit & 1;
}

static int
hevc_skip(hevc_bits *b, int n)
{
    while (n-- > 0)
        if (hevc_bit(b) < 0)
            return -1;
    return 0;
}

static int
hevc_ue(hevc_bits *b)
{
    int zeros = 0, v = 0, bit;

    while ((bit = hevc_bit(b)) == 0)
        if (++zeros > 31)
            return -1;
    if (bit < 0)
        return -1;
    for (int i = 0; i < zeros; i++) {
        bit = hevc_bit(b);
        if (bit < 0)
            return -1;
        v = v << 1 | bit;
    }
    return (1 << zeros) - 1 + v;
}

/* chroma_format_idc of an SPS (H.265 7.3.2.2), from its payload after the
 * two-byte NAL header; -1 when cut short. */
static int
hevc_chroma_of_sps(const uint8_t *p, size_t n)
{
    hevc_bits b = { p, n, 0, 0, 0, 0 };
    int max_sub_layers, sub_profile[8] = { 0 }, sub_level[8] = { 0 };

    if (hevc_skip(&b, 4) < 0) /* sps_video_parameter_set_id */
        return -1;
    max_sub_layers = 0;
    for (int i = 0; i < 3; i++) {
        int bit = hevc_bit(&b);

        if (bit < 0)
            return -1;
        max_sub_layers = max_sub_layers << 1 | bit;
    }
    /* temporal_id_nesting, then profile_tier_level's general part (88 bits)
     * and general_level_idc */
    if (hevc_skip(&b, 1 + 88 + 8) < 0)
        return -1;
    for (int i = 0; i < max_sub_layers; i++) {
        sub_profile[i] = hevc_bit(&b);
        sub_level[i] = hevc_bit(&b);
        if (sub_profile[i] < 0 || sub_level[i] < 0)
            return -1;
    }
    if (max_sub_layers > 0 && hevc_skip(&b, 2 * (8 - max_sub_layers)) < 0)
        return -1;
    for (int i = 0; i < max_sub_layers; i++)
        if (hevc_skip(&b, (sub_profile[i] ? 88 : 0) + (sub_level[i] ? 8 : 0)) < 0)
            return -1;
    if (hevc_ue(&b) < 0) /* sps_seq_parameter_set_id */
        return -1;
    return hevc_ue(&b);
}

/* The first SPS (NAL type 33) in Annex B data. */
static int
hevc_sps_chroma(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 5 < n; i++) {
        if (p[i] || p[i + 1] || p[i + 2] != 1)
            continue;
        if ((p[i + 3] >> 1 & 0x3f) == 33) {
            size_t end = i + 3;

            while (end + 2 < n && !(p[end] == 0 && p[end + 1] == 0 && p[end + 2] <= 1))
                end++;
            if (end + 2 >= n)
                end = n;
            return hevc_chroma_of_sps(p + i + 5, end - (i + 5));
        }
        i += 2;
    }
    return -1;
}

static int
hevc_supports(const papaya_stream_info *s)
{
    return s->codec == PAPAYA_CODEC_HEVC;
}

const papaya_decode_backend papaya_decode_hevc = {
    "libhevc", hevc_supports, itt_open, itt_decode, itt_flush, itt_close,
};
