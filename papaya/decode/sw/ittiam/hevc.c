/* HEVC through libhevc (Apache-2.0, Android's software decoder). */
#include "decode_internal.h"
#include "ihevc_typedefs.h"
#include "iv.h"
#include "ivd.h"
#include "ihevcd_cxa.h"

#define ITT_PREFIX(x) ihevcd_cxa_##x
#define ITT_API ihevcd_cxa_api_function
#define ITT_CORES_CMD IHEVCD_CXA_CMD_CTL_SET_NUM_CORES
#define ITT_CREATE_EXTRA(ip) ((void)0)

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

static int
hevc_supports(const papaya_stream_info *s)
{
    return s->codec == PAPAYA_CODEC_HEVC;
}

const papaya_decode_backend papaya_decode_hevc = {
    "libhevc", hevc_supports, itt_open, itt_decode, itt_flush, itt_close,
};
