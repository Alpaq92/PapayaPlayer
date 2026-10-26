#include <string.h>

#include "FLAC/format.h"
#include "dav1d/dav1d.h"
#include "greatest.h"
#include "miniaudio.h"
#include "ogg/ogg.h"
#include "opus.h"
#include "vorbis/codec.h"
#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"

/* Every third-party library links and answers. */
TEST
libraries_link(void)
{
    ogg_sync_state oy;

    ASSERT_EQ(0, ogg_sync_init(&oy));
    ogg_sync_clear(&oy);
    ASSERT(strstr(opus_get_version_string(), "libopus") != NULL);
    ASSERT(vorbis_version_string() != NULL);
    ASSERT(FLAC__VERSION_STRING != NULL);
    ASSERT(ma_version_string() != NULL);
    ASSERT(strncmp(dav1d_version(), "1.", 2) == 0);
    ASSERT(strncmp(vpx_codec_version_str(), "v1.", 3) == 0);
    ASSERT(vpx_codec_vp9_dx() != NULL && vpx_codec_vp8_dx() != NULL);
    PASS();
}

GREATEST_MAIN_DEFS();

int
main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(libraries_link);
    GREATEST_MAIN_END();
}
