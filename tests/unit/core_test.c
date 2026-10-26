#include <string.h>

#include "greatest.h"
#include "papaya/version.h"

TEST
version_matches_header(void)
{
    ASSERT_STR_EQ(PAPAYA_VERSION_STRING, papaya_version());
    ASSERT_EQ(PAPAYA_VERSION_MAJOR * 10000 + PAPAYA_VERSION_MINOR * 100 + PAPAYA_VERSION_PATCH,
              papaya_version_number());
    PASS();
}

SUITE(core)
{
    RUN_TEST(version_matches_header);
}

GREATEST_MAIN_DEFS();

int
main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(core);
    GREATEST_MAIN_END();
}
