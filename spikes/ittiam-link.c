/* Spike S2: links one Ittiam decoder and checks its entry point resolves. */
#include <stdio.h>

int PAPAYA_ENTRY(void *handle, void *in, void *out);

int
main(void)
{
    int (*volatile entry)(void *, void *, void *) = PAPAYA_ENTRY;

    printf("decoder entry at %p\n", (void *)entry);
    return entry ? 0 : 1;
}
