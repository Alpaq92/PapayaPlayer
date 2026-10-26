/* Spike S3: video frames on Reaktor's renderer. Draws an NV12 frame (BT.709,
 * limited range) and a P010 frame (HDR10: BT.2020 + PQ) as SDL streaming
 * textures through Nuklear's image path. P010 needs a YCbCr colorspace built
 * with SDL_DEFINE_COLORSPACE. Run with a GPU renderer:
 *   spike_video_texture --renderer direct3d11 --shot s3.bmp
 * The left half should show a grey ramp tinted by a colour sweep; the right
 * half an HDR ramp that SDL tone-maps when the output is SDR. */
#include <stdint.h>
#include <stdio.h>

#include "reaktor/reaktor.h"
#include "reaktor/main.h"

enum { W = 640, H = 360 };

static SDL_Texture *nv12, *p010;
static const char *status = "";

static SDL_Texture *
make_texture(SDL_Renderer *r, SDL_PixelFormat fmt, SDL_Colorspace cs)
{
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_Texture *t;

    SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, fmt);
    SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, cs);
    SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STREAMING);
    SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, W);
    SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, H);
    t = SDL_CreateTextureWithProperties(r, p);
    SDL_DestroyProperties(p);
    return t;
}

static void
fill_nv12(SDL_Texture *t)
{
    static uint8_t y[W * H], uv[W * H / 2];

    for (int j = 0; j < H; j++)
        for (int i = 0; i < W; i++)
            y[j * W + i] = (uint8_t)(16 + i * 219 / (W - 1));
    for (int j = 0; j < H / 2; j++)
        for (int i = 0; i < W / 2; i++) {
            uv[j * W + 2 * i] = (uint8_t)(16 + j * 224 / (H / 2 - 1));
            uv[j * W + 2 * i + 1] = (uint8_t)(240 - i * 224 / (W / 2 - 1));
        }
    SDL_UpdateNVTexture(t, NULL, y, W, uv, W);
}

static void
fill_p010(SDL_Texture *t)
{
    static uint16_t y[W * H], uv[W * H / 2];

    /* PQ code values 64..940 (10-bit limited range) across the width, i.e.
     * 0 to 10000 nits; P010 keeps the 10 bits in the high bits. */
    for (int j = 0; j < H; j++)
        for (int i = 0; i < W; i++)
            y[j * W + i] = (uint16_t)((64 + i * 876 / (W - 1)) << 6);
    for (int j = 0; j < H / 2; j++)
        for (int i = 0; i < W; i++)
            uv[j * W + i] = (uint16_t)(512 << 6);
    SDL_UpdateNVTexture(t, NULL, (const uint8_t *)y, W * 2, (const uint8_t *)uv, W * 2);
}

static void
draw(struct reaktor_context *ctx, SDL_Texture *t, float x, float y, float w, float h)
{
    struct reaktor_image im = {0};

    if (!t)
        return;
    im.handle.ptr = t;
    im.w = W;
    im.h = H;
    im.region[2] = W;
    im.region[3] = H;
    reaktor_draw_image(reaktor_window_get_canvas(ctx), reaktor_rect(x, y, w, h), &im,
                       reaktor_rgb(255, 255, 255));
}

static void
page(App *app, struct reaktor_context *ctx, int win_w, int win_h)
{
    static char line[256];

    if (!nv12 && !p010) {
        SDL_Renderer *r = reaktor_sdl_renderer(app);

        static char p010_err[128];

        nv12 = make_texture(r, SDL_PIXELFORMAT_NV12, SDL_COLORSPACE_BT709_LIMITED);
        /* SDL_COLORSPACE_HDR10 is the RGB one; YUV HDR10 is defined here. */
        p010 = make_texture(r, SDL_PIXELFORMAT_P010,
                            SDL_DEFINE_COLORSPACE(SDL_COLOR_TYPE_YCBCR, SDL_COLOR_RANGE_LIMITED,
                                                  SDL_COLOR_PRIMARIES_BT2020,
                                                  SDL_TRANSFER_CHARACTERISTICS_PQ,
                                                  SDL_MATRIX_COEFFICIENTS_BT2020_NCL,
                                                  SDL_CHROMA_LOCATION_LEFT));
        snprintf(p010_err, sizeof p010_err, "%s", p010 ? "ok" : SDL_GetError());
        if (nv12)
            fill_nv12(nv12);
        if (p010)
            fill_p010(p010);
        snprintf(line, sizeof line, "renderer %s  NV12 %s  P010 %s  output colorspace %lld",
                 SDL_GetRendererName(r), nv12 ? "ok" : "failed", p010_err,
                 (long long)SDL_GetNumberProperty(SDL_GetRendererProperties(r),
                                                  SDL_PROP_RENDERER_OUTPUT_COLORSPACE_NUMBER, 0));
        status = line;
        SDL_Log("%s", status);
    }
    REAKTOR_FREE(.w = (float)win_w, .h = (float)win_h) {
        reaktor_label(&(reaktor_label_spec){
            .text = status, .name = "status",
            .box = { .w = (float)win_w, .h = 30.0f } });
        draw(ctx, nv12, 0.0f, 40.0f, (float)win_w / 2.0f, (float)win_h - 40.0f);
        draw(ctx, p010, (float)win_w / 2.0f, 40.0f, (float)win_w / 2.0f, (float)win_h - 40.0f);
    }
}

int
main(int argc, char **argv)
{
    return launchApp(argc, argv, &(reaktor_launch){
        .name   = "Spike S3",
        .window = { .w = 1280, .h = 400 },
        .page   = page });
}
