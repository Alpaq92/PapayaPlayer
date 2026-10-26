#include "papaya/version.h"

const char *
papaya_version(void)
{
    return PAPAYA_VERSION_STRING;
}

int
papaya_version_number(void)
{
    return PAPAYA_VERSION_MAJOR * 10000 + PAPAYA_VERSION_MINOR * 100 + PAPAYA_VERSION_PATCH;
}
