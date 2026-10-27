/* HEVC through libhevc (Apache-2.0, Android's software decoder). */
#include "decode_internal.h"
#include "ihevc_typedefs.h"
#include "iv.h"
#include "ivd.h"
#include "ihevcd_cxa.h"

#define ITT_PREFIX(x) ihevcd_cxa_##x
#define ITT_API ihevcd_cxa_api_function
#define ITT_CORES_CMD IHEVCD_CXA_CMD_CTL_SET_NUM_CORES
/* Main 10: 16-bit output samples. */
#define ITT_CREATE_EXTRA(ip) ((ip).u4_enable_hbd = 1)
#define ITT_BIT_DEPTH(op) ((op)->u4_bit_depth)

/* HDR in a raw stream (MPEG-TS, Annex B) is known only from the bitstream:
 * the SPS's VUI and the mastering display SEI. */
static void hevc_stream_color(void *ctx);
static void hevc_picture_color(void *ctx, papaya_frame *f);
#define ITT_STREAM_COLOR(c) hevc_stream_color(c)
#define ITT_PICTURE_COLOR(c, f) hevc_picture_color(c, f)

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

static int
hevc_supports(const papaya_stream_info *s)
{
    return s->codec == PAPAYA_CODEC_HEVC;
}

const papaya_decode_backend papaya_decode_hevc = {
    "libhevc", hevc_supports, itt_open, itt_decode, itt_flush, itt_close,
};
