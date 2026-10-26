/* H.264 through libavc (Apache-2.0, Android's software decoder). */
#include "decode_internal.h"
#include "ih264_typedefs.h"
#include "iv.h"
#include "ivd.h"
#include "ih264d.h"

#define ITT_PREFIX(x) ih264d_##x
#define ITT_API ih264d_api_function
#define ITT_CORES_CMD IH264D_CMD_CTL_SET_NUM_CORES
#define ITT_CREATE_EXTRA(ip) ((void)0)

#include "ittiam_video.h"

/* avcC (ISO/IEC 14496-15 5.3.3.1): parameter sets and the NAL length size.
 * Anything else is taken as Annex B. */
static int
itt_to_annexb_config(itt_ctx *c, const uint8_t *p, size_t n)
{
    size_t at = 6;
    int sps;

    if (n < 7 || p[0] != 1)
        return 0;
    c->length_size = (p[4] & 3) + 1;
    sps = p[5] & 0x1f;
    for (int set = 0; set < 2; set++) {
        int count = set ? (at < n ? p[at++] : 0) : sps;

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
avc_supports(const papaya_stream_info *s)
{
    return s->codec == PAPAYA_CODEC_H264;
}

const papaya_decode_backend papaya_decode_avc = {
    "libavc", avc_supports, itt_open, itt_decode, itt_flush, itt_close,
};
