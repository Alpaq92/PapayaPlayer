#include <string.h>

#include "FLAC/format.h"
#include "greatest.h"
#include "miniaudio.h"
#include "ogg/ogg.h"
#include "opus.h"
#include "vorbis/codec.h"

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
