#include <stdio.h>

#include "reaktor/reaktor.h"
#include "reaktor/main.h"
#include "papaya/version.h"

/* Phase 0 placeholder window: proves Reaktor, SDL and papaya_core build and
 * link together. The player UI arrives in phase 3. */
static void
page_shell(App *app, struct reaktor_context *ctx, int win_w, int win_h)
{
    static char title[64];

    (void)app;
    (void)ctx;
    snprintf(title, sizeof title, "PapayaPlayer %s", papaya_version());
    REAKTOR_FREE(.w = (float)win_w, .h = (float)win_h) {
        reaktor_label(&(reaktor_label_spec){
            .text = title,
            .name = "title",
            .align = REAKTOR_TEXT_CENTERED,
            .box  = { .w = (float)win_w, .h = 40.0f, .mt = ((float)win_h - 40.0f) * 0.5f } });
    }
}

static int
player_key(App *app, const SDL_Event *e)
{
    if (reaktor_chord(e, 0, SDLK_ESCAPE)) {
        reaktor_request_quit(app);
        return 1;
    }
    return 0;
}

int
main(int argc, char **argv)
{
    return launchApp(argc, argv, &(reaktor_launch){
        .name    = "PapayaPlayer",
        .version = PAPAYA_VERSION_STRING,
        .window  = { .w = 960, .h = 540 },
        .page    = page_shell,
        .key     = player_key });
}
