#include "greatest.h"
#include "papaya_hello.h"

TEST
rust_crate_links(void)
{
    ASSERT_EQ(42u, papaya_rust_hello());
    PASS();
}

GREATEST_MAIN_DEFS();

int
main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(rust_crate_links);
    GREATEST_MAIN_END();
}
