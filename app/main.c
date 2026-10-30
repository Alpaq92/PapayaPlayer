/* PapayaPlayer's window: the video scaled to fit, a top bar choosing the
 * folder and the file in it, and a control bar with play / pause, -10 / +10,
 * previous / next, a seek slider, the time and the volume. Both bars hide while
 * playing. The designed UI comes in phase 3.
 *
 * Keys: Space play / pause, Left / Right seek 5 s, J / L seek 10 s,
 * P / N previous / next file, M mute, F full screen, O open, Escape close
 * settings, leave full screen or quit.
 *
 * Settings (the cog, right end of the control bar): native or custom title bar, and whether playback moves
 * on to the next file. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "papaya/player.h"
#include "papaya/spectrum.h"
#include "papaya/thread.h"
#include "papaya/version.h"
#include "playlist.h"
#include "plutovg.h"
#include "reaktor/main.h"
#include "reaktor/reaktor.h"

#define BAR_H 80.0f  /* the seek bar, then the buttons */
#define SEEK_H 28.0f
#define VIS_BANDS 48
enum { VIS_BARS, VIS_BLOCKS, VIS_WAVE, VIS_OFF };
enum { VIS_GRADIENT, VIS_ACCENT, VIS_HUE };
#define SEEK_MARGIN 20.0f /* the seek bar's distance from the bar's sides */
#define ACCENT_INSET 14.0f  /* the accent bar's distance from the window edges */
#define TOP_H 52.0f

#ifndef PAPAYA_ICON_DIR
#define PAPAYA_ICON_DIR NULL
#endif

static struct {
    App *app;
    papaya_player *player;
    char title[256];
    char error[512];

    SDL_Texture *texture;
    papaya_pixel_format tex_pix;
    int tex_w, tex_h;
    SDL_Colorspace tex_cs;
    int no_p010;             /* renderer lacks P010: show 10-bit as 8-bit */
    uint16_t *p010;          /* 10-bit frames repacked for SDL */
    size_t p010_size;
    uint8_t *chroma420;      /* 4:2:2 / 4:4:4 chroma averaged down to 4:2:0 */
    size_t chroma420_size;

    unsigned timer;
    float seek_value;        /* slider position, seconds */

    playlist list;           /* the folder of the file playing */
    char recent[RECENT_MAX][1024];
    int recent_count;
    int menu_open;           /* a top-bar dropdown is open: keep the bars */
    float volume;            /* 0..1, kept across files */
    int muted;

    /* settings, kept in Reaktor's settings file */
    int custom_titlebar;     /* borderless window, the top bar is the title bar */
    reaktor_bool autoplay_next;
    int accent_style;        /* controls: 0 minimal, 1 accent (a floating, outlined bar) */
    int pip;                 /* picture-in-picture: a small window on top */
    int pip_x, pip_y, pip_w, pip_h; /* the window before it */
    SDL_Texture *cover;      /* the file's embedded picture, or NULL */
    int cover_w, cover_h;
    int vis_style;           /* visualiser: VIS_BARS ... VIS_OFF */
    int vis_colour;          /* VIS_GRADIENT, VIS_ACCENT, VIS_HUE */
    papaya_spectrum *vis;
    float vis_levels[VIS_BANDS];
    int64_t vis_at;          /* when the levels were last updated */
    int settings_tab;
    int settings_open;
    int playlist_open;       /* the slide-out list of the folder's files */
    struct reaktor_rect playlist_rect, list_button_rect;
    struct reaktor_rect settings_rect, cog_rect;
    struct reaktor_rect drag_rect;  /* the top bar's empty middle, for the hit test */
    int bars_shown;
} ui;

/* ---- textures ----------------------------------------------------------- */

static SDL_Colorspace
colorspace_of(const papaya_frame *f)
{
    SDL_MatrixCoefficients m = f->color.matrix == 9 ? SDL_MATRIX_COEFFICIENTS_BT2020_NCL
                             : f->color.matrix == 1 ? SDL_MATRIX_COEFFICIENTS_BT709
                             : f->color.matrix == 2 && f->height > 576 ? SDL_MATRIX_COEFFICIENTS_BT709
                             : SDL_MATRIX_COEFFICIENTS_BT601;
    SDL_ColorPrimaries pri = f->color.primaries == 9 ? SDL_COLOR_PRIMARIES_BT2020 : SDL_COLOR_PRIMARIES_BT709;
    SDL_TransferCharacteristics trc = f->color.transfer == 16 ? SDL_TRANSFER_CHARACTERISTICS_PQ
                                    : f->color.transfer == 18 ? SDL_TRANSFER_CHARACTERISTICS_HLG
                                    : SDL_TRANSFER_CHARACTERISTICS_BT709;

    return SDL_DEFINE_COLORSPACE(SDL_COLOR_TYPE_YCBCR,
                                 f->color.full_range ? SDL_COLOR_RANGE_FULL : SDL_COLOR_RANGE_LIMITED,
                                 pri, trc, m, SDL_CHROMA_LOCATION_LEFT);
}

static int
ensure_texture(const papaya_frame *f, SDL_Colorspace cs)
{
    SDL_PixelFormat fmt = f->pix == PAPAYA_PIX_I420_10 && !ui.no_p010 ? SDL_PIXELFORMAT_P010
                                                                      : SDL_PIXELFORMAT_IYUV;
    SDL_PropertiesID props;

    if (ui.texture && ui.tex_pix == f->pix && ui.tex_w == f->width && ui.tex_h == f->height && ui.tex_cs == cs)
        return 0;
    if (ui.texture)
        SDL_DestroyTexture(ui.texture);
    props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, fmt);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, cs);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STREAMING);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, f->width);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, f->height);
    if (SDL_COLORSPACETRANSFER(cs) == SDL_TRANSFER_CHARACTERISTICS_PQ) {
        /* HDR10: diffuse white at 203 cd/m2 (ITU-R BT.2408), and the content's
         * peak as headroom, so SDL scales highlights into what the screen
         * has instead of clipping them. Without metadata, 1000 cd/m2, the
         * usual mastering peak. */
        float peak = f->color.max_cll ? (float)f->color.max_cll
                     : f->color.max_luminance > 0 ? f->color.max_luminance : 1000.0f;

        SDL_SetFloatProperty(props, SDL_PROP_TEXTURE_CREATE_SDR_WHITE_POINT_FLOAT, 203.0f);
        SDL_SetFloatProperty(props, SDL_PROP_TEXTURE_CREATE_HDR_HEADROOM_FLOAT, peak > 203.0f ? peak / 203.0f : 1.0f);
    }
    ui.texture = SDL_CreateTextureWithProperties(reaktor_sdl_renderer(ui.app), props);
    SDL_DestroyProperties(props);
    if (!ui.texture && fmt == SDL_PIXELFORMAT_P010) {
        ui.no_p010 = 1; /* e.g. the software renderer */
        return ensure_texture(f, SDL_COLORSPACE_BT709_LIMITED);
    }
    if (!ui.texture) {
        snprintf(ui.error, sizeof ui.error, "cannot show this video: %s", SDL_GetError());
        return -1;
    }
    ui.tex_pix = f->pix;
    ui.tex_w = f->width;
    ui.tex_h = f->height;
    ui.tex_cs = cs;
    return 0;
}

/* 10-bit planar to 8-bit IYUV, when the renderer has no P010. */
static void
upload_10bit_as_8bit(const papaya_frame *f)
{
    int cw = (f->width + 1) / 2, ch = (f->height + 1) / 2;
    size_t need = ((size_t)f->width * (size_t)f->height + (size_t)cw * (size_t)ch * 2 + 1) / 2;
    uint8_t *p8, *u8, *v8;

    if (ui.p010_size < need) {
        free(ui.p010);
        ui.p010 = malloc(need * sizeof *ui.p010);
        ui.p010_size = ui.p010 ? need : 0;
        if (!ui.p010)
            return;
    }
    p8 = (uint8_t *)ui.p010;
    u8 = p8 + (size_t)f->width * (size_t)f->height;
    v8 = u8 + (size_t)cw * (size_t)ch;
    for (int pl = 0; pl < 3; pl++) {
        int pw = pl ? cw : f->width, ph = pl ? ch : f->height;
        uint8_t *dst = pl == 0 ? p8 : pl == 1 ? u8 : v8;

        for (int y = 0; y < ph; y++) {
            const uint16_t *src = (const uint16_t *)(f->planes[pl] + (size_t)y * (size_t)f->strides[pl]);

            for (int x = 0; x < pw; x++)
                dst[(size_t)y * (size_t)pw + (size_t)x] = (uint8_t)(src[x] >> 2);
        }
    }
    SDL_UpdateYUVTexture(ui.texture, NULL, p8, f->width, u8, cw, v8, cw);
}

/* 10-bit planar (low bits) to P010 (interleaved chroma, high bits). */
static void
upload_10bit(const papaya_frame *f)
{
    int cw = (f->width + 1) / 2, ch = (f->height + 1) / 2;
    size_t y_size = (size_t)f->width * (size_t)f->height, need = y_size + (size_t)cw * 2 * (size_t)ch;

    if (f->width <= 0 || f->height <= 0)
        return;
    uint16_t *uv;

    if (ui.p010_size < need) {
        free(ui.p010);
        ui.p010 = malloc(need * sizeof *ui.p010);
        ui.p010_size = ui.p010 ? need : 0;
        if (!ui.p010)
            return;
    }
    for (int y = 0; y < f->height; y++) {
        const uint16_t *src = (const uint16_t *)(f->planes[0] + (size_t)y * (size_t)f->strides[0]);
        uint16_t *dst = ui.p010 + (size_t)y * (size_t)f->width;

        for (int x = 0; x < f->width; x++)
            dst[x] = (uint16_t)(src[x] << 6);
    }
    uv = ui.p010 + y_size;
    for (int y = 0; y < ch; y++) {
        const uint16_t *u = (const uint16_t *)(f->planes[1] + (size_t)y * (size_t)f->strides[1]);
        const uint16_t *v = (const uint16_t *)(f->planes[2] + (size_t)y * (size_t)f->strides[2]);
        uint16_t *dst = uv + (size_t)y * (size_t)cw * 2;

        for (size_t x = 0; x < (size_t)cw; x++) {
            dst[2 * x] = (uint16_t)(u[x] << 6);
            dst[2 * x + 1] = (uint16_t)(v[x] << 6);
        }
    }
    SDL_UpdateNVTexture(ui.texture, NULL, (const Uint8 *)ui.p010, f->width * 2, (const Uint8 *)uv, cw * 4);
}

/* A 4:2:0 view of a 4:2:2 / 4:4:4 frame: the same luma, chroma averaged
 * over 1x2 or 2x2 samples. SDL has no planar 4:2:2 / 4:4:4 textures; full
 * chroma waits for our own shaders. Returns 0, or -1 without memory. */
static int
to_420(const papaya_frame *f, papaya_frame *out)
{
    int ten = papaya_pix_bytes(f->pix) == 2;
    int sx = f->pix == PAPAYA_PIX_I444 || f->pix == PAPAYA_PIX_I444_10 ? 2 : 1;
    int src_w = sx == 2 ? f->width : (f->width + 1) / 2;
    int cw = (f->width + 1) / 2, ch = (f->height + 1) / 2;
    size_t row = (size_t)cw << ten, need = row * (size_t)ch * 2;

    if (ui.chroma420_size < need) {
        free(ui.chroma420);
        ui.chroma420 = malloc(need);
        ui.chroma420_size = ui.chroma420 ? need : 0;
        if (!ui.chroma420)
            return -1;
    }
    *out = *f;
    out->pix = ten ? PAPAYA_PIX_I420_10 : PAPAYA_PIX_I420;
    for (int pl = 1; pl <= 2; pl++) {
        uint8_t *dst = ui.chroma420 + (size_t)(pl - 1) * row * (size_t)ch;

        out->planes[pl] = dst;
        out->strides[pl] = (int)row;
        for (int y = 0; y < ch; y++) {
            int y1 = 2 * y + 1 < f->height ? 2 * y + 1 : 2 * y;
            const uint8_t *r0 = f->planes[pl] + (size_t)(2 * y) * (size_t)f->strides[pl];
            const uint8_t *r1 = f->planes[pl] + (size_t)y1 * (size_t)f->strides[pl];

            for (int x = 0; x < cw; x++) {
                int x0 = x * sx, x1 = sx == 2 && x0 + 1 < src_w ? x0 + 1 : x0;
                unsigned sum;

                if (ten) {
                    const uint16_t *a = (const uint16_t *)r0, *b = (const uint16_t *)r1;

                    sum = (unsigned)a[x0] + a[x1] + b[x0] + b[x1];
                    ((uint16_t *)(dst + (size_t)y * row))[x] = (uint16_t)((sum + 2) >> 2);
                } else {
                    sum = (unsigned)r0[x0] + r0[x1] + r1[x0] + r1[x1];
                    dst[(size_t)y * row + (size_t)x] = (uint8_t)((sum + 2) >> 2);
                }
            }
        }
    }
    return 0;
}

static void
show_frame(papaya_frame *frame)
{
    papaya_frame view;
    const papaya_frame *f = frame;

    if (f->pix == PAPAYA_PIX_I422 || f->pix == PAPAYA_PIX_I444 || f->pix == PAPAYA_PIX_I422_10
        || f->pix == PAPAYA_PIX_I444_10) {
        if (to_420(f, &view) != 0)
            return;
        f = &view;
    }
    if (f->pix != PAPAYA_PIX_I420 && f->pix != PAPAYA_PIX_I420_10) {
        snprintf(ui.error, sizeof ui.error, "cannot show this pixel format");
        return;
    }
    if (ensure_texture(f, ui.no_p010 && f->pix == PAPAYA_PIX_I420_10 ? SDL_COLORSPACE_BT709_LIMITED
                                                                     : colorspace_of(f)) != 0)
        return;
    if (f->pix == PAPAYA_PIX_I420_10 && ui.no_p010)
        upload_10bit_as_8bit(f);
    else if (f->pix == PAPAYA_PIX_I420)
        SDL_UpdateYUVTexture(ui.texture, NULL, f->planes[0], f->strides[0], f->planes[1], f->strides[1],
                             f->planes[2], f->strides[2]);
    else
        upload_10bit(f);
}

/* ---- playback ----------------------------------------------------------- */

static void
wake_ui(void *user)
{
    reaktor_wake(user);
}

/* While playing, the window redraws often enough to show each frame on time. */
static void
tick(App *app, void *user)
{
    (void)user;
    reaktor_wake(app);
}

static void
close_file(void)
{
    if (ui.timer) {
        reaktor_timer_stop(ui.timer);
        ui.timer = 0;
    }
    papaya_player_close(&ui.player);
    if (ui.texture) {
        SDL_DestroyTexture(ui.texture);
        ui.texture = NULL;
    }
    if (ui.cover) {
        SDL_DestroyTexture(ui.cover);
        ui.cover = NULL;
    }
}

/* The file's embedded picture as a texture, for the music view. */
static void
load_cover(void)
{
    size_t size;
    const uint8_t *data = papaya_player_cover(ui.player, &size);
    plutovg_surface_t *s;

    if (!data || size > INT32_MAX)
        return;
    s = plutovg_surface_load_from_image_data(data, (int)size);
    if (!s)
        return;
    ui.cover_w = plutovg_surface_get_width(s);
    ui.cover_h = plutovg_surface_get_height(s);
    /* premultiplied ARGB in native order, as SDL's ARGB8888 */
    ui.cover = SDL_CreateTexture(reaktor_sdl_renderer(ui.app), SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
                                 ui.cover_w, ui.cover_h);
    if (ui.cover) {
        SDL_UpdateTexture(ui.cover, NULL, plutovg_surface_get_data(s), plutovg_surface_get_stride(s));
        SDL_SetTextureBlendMode(ui.cover, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    }
    plutovg_surface_destroy(s);
}

static void
set_paused(int paused)
{
    if (!ui.player)
        return;
    papaya_player_set_paused(ui.player, paused);
    if (!paused && !ui.timer)
        ui.timer = reaktor_timer(ui.app, 4, 1, tick, NULL);
    else if (paused && ui.timer) {
        reaktor_timer_stop(ui.timer);
        ui.timer = 0;
    }
}

static void touch(void);
static void apply_volume(void);

static void
play_path(const char *path)
{
    papaya_player_config cfg = { .wake = wake_ui, .wake_user = ui.app };
    const char *name = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');

    close_file();
    ui.error[0] = 0;
    touch(); /* the bar shows for a moment on every new file */
    ui.player = papaya_player_open(path, &cfg);
    if (!ui.player) {
        snprintf(ui.error, sizeof ui.error, "Cannot play %s", path);
        return;
    }
    apply_volume();
    load_cover();
    if (bslash && (!name || bslash > name))
        name = bslash;
    snprintf(ui.title, sizeof ui.title, "%s", name ? name + 1 : path);
    /* Give the first picture up to a second, so the window opens on it. */
    for (int i = 0; i < 200 && papaya_player_video_stream(ui.player)
                    && papaya_player_next_frame_in(ui.player) < 0; i++)
        SDL_Delay(5);
    set_paused(0);
}

/* ---- playlist ----------------------------------------------------------- */

/* Handler arguments: seconds to seek, files to step. */
static int step_back10 = -10, step_fwd10 = 10, step_prev = -1, step_next = 1;

static void
play_index(int i)
{
    char path[2048];

    if (i < 0 || i >= ui.list.count)
        return;
    ui.list.current = i;
    playlist_path(&ui.list, i, path, sizeof path);
    play_path(path);
}

static void
remember_folder(void)
{
    recent_add(ui.list.dir);
    ui.recent_count = recent_folders(ui.recent);
}

/* A file from anywhere: its folder becomes the playlist. */
static void
open_file(const char *path)
{
    if (playlist_load_file(&ui.list, path) == 0 && ui.list.current >= 0) {
        remember_folder();
        play_index(ui.list.current);
    } else {
        playlist_free(&ui.list);
        ui.list.dir[0] = ui.list.name[0] = 0;
        play_path(path);
    }
}

static void
open_folder(const char *dir)
{
    if (playlist_load(&ui.list, dir, NULL) != 0) {
        snprintf(ui.error, sizeof ui.error, "Cannot read %s", dir);
        return;
    }
    remember_folder();
    if (ui.list.count > 0)
        play_index(0);
    else {
        close_file();
        snprintf(ui.error, sizeof ui.error, "No playable files in %s", ui.list.name);
    }
}

static void
step_file(void *user)
{
    play_index(ui.list.current + *(const int *)user);
}

static void
toggle_pause(void *user)
{
    (void)user;
    if (ui.player) {
        if (papaya_player_ended(ui.player))
            papaya_player_seek(ui.player, 0);
        set_paused(!papaya_player_paused(ui.player));
    }
}

static void
seek_by(int64_t delta)
{
    if (ui.player) {
        int64_t to = papaya_player_position(ui.player) + delta;

        papaya_player_seek(ui.player, to < 0 ? 0 : to);
    }
}

static void
seek_button(void *user)
{
    seek_by((int64_t)*(const int *)user * 1000000);
    touch();
}

static void
chosen(App *app, const char *const *paths, int count, void *user)
{
    (void)app;
    (void)user;
    if (count > 0 && paths[0])
        open_file(paths[0]);
}

static void
chosen_folder(App *app, const char *const *paths, int count, void *user)
{
    (void)app;
    (void)user;
    if (count > 0 && paths[0])
        open_folder(paths[0]);
}

static void
ask_for_folder(void *user)
{
    (void)user;
    reaktor_file_dialog_open(ui.app, &(reaktor_file_dialog){
        .kind = REAKTOR_FOLDER_OPEN, .location = ui.list.dir[0] ? ui.list.dir : NULL,
        .chosen = chosen_folder });
}

static void
ask_for_file(void *user)
{
    static const reaktor_file_filter filters[] = {
        { "Media files", "webm;mkv;mka;ivf;mp4;m4v;m4a;mov" },
        { "All files", "*" },
    };

    (void)user;
    reaktor_file_dialog_open(ui.app, &(reaktor_file_dialog){
        .filters = filters, .filter_count = 2, .location = ui.list.dir[0] ? ui.list.dir : NULL,
        .chosen = chosen });
}

/* ---- page --------------------------------------------------------------- */

#define HIDE_AFTER_US 2500000

static struct {
    float mouse_x, mouse_y;
    int64_t active_at;       /* last mouse move or key, papaya_time_us() */
    int win_w, win_h;        /* the size the last frame was laid out for */
} activity;

static void
touch(void)
{
    activity.active_at = papaya_time_us();
}

static void
format_time(char *out, size_t size, int64_t us)
{
    int s = us > 0 ? (int)(us / 1000000) : 0;

    if (s >= 3600)
        snprintf(out, size, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else
        snprintf(out, size, "%d:%02d", s / 60, s % 60);
}

static void
draw_video(struct reaktor_context *ctx, float w, float h)
{
    struct reaktor_image im = { 0 };
    float scale, vw, vh;

    reaktor_fill_rect(reaktor_window_get_canvas(ctx), reaktor_rect(0, 0, w, h), 0, reaktor_rgb(0, 0, 0));
    if (!ui.texture || ui.tex_w <= 0 || ui.tex_h <= 0)
        return;
    scale = w / (float)ui.tex_w < h / (float)ui.tex_h ? w / (float)ui.tex_w : h / (float)ui.tex_h;
    vw = (float)ui.tex_w * scale;
    vh = (float)ui.tex_h * scale;
    im.handle.ptr = ui.texture;
    im.w = (unsigned short)ui.tex_w;
    im.h = (unsigned short)ui.tex_h;
    im.region[2] = (unsigned short)ui.tex_w;
    im.region[3] = (unsigned short)ui.tex_h;
    reaktor_draw_image(reaktor_window_get_canvas(ctx), reaktor_rect((w - vw) / 2, (h - vh) / 2, vw, vh), &im,
                       reaktor_rgb(255, 255, 255));
}

/* A click anywhere on the seek track jumps there (Nuklear's slider only
 * steps toward a click); dragging stays the slider's own. */
static void
click_to_seek(struct reaktor_context *ctx, struct reaktor_rect track, int64_t duration)
{
    const struct nk_mouse_button *left = &ctx->input.mouse.buttons[NK_BUTTON_LEFT];
    float x = ctx->input.mouse.pos.x;

    if (!ui.player || duration <= 0 || track.w <= 0.0f || !left->clicked || !left->down
        || !reaktor_input_is_mouse_hovering_rect(&ctx->input, track))
        return;
    x = (x - track.x) / track.w;
    x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
    ui.seek_value = x * (float)duration / 1e6f;
    papaya_player_seek(ui.player, (int64_t)((double)x * (double)duration));
    touch();
}

static int
fullscreen(void)
{
    return (SDL_GetWindowFlags(reaktor_sdl_window(ui.app)) & SDL_WINDOW_FULLSCREEN) != 0;
}

static void
toggle_fullscreen(void *user)
{
    (void)user;
    SDL_SetWindowFullscreen(reaktor_sdl_window(ui.app), !fullscreen());
    touch();
}

/* The play pill's icon in the stylesheet's ink (dark on papaya): two bars
 * while playing, else a triangle. */
static void
paint_play(App *app, struct reaktor_command_buffer *cv, struct reaktor_rect r, struct reaktor_color ink, void *user)
{
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f, s = 7.0f;
    struct nk_color c = nk_rgba(ink.r, ink.g, ink.b, ink.a);

    (void)app;
    if (*(const int *)user) {
        nk_fill_rect(cv, nk_rect(cx - s * 0.8f, cy - s, s * 0.55f, 2.0f * s), 1.0f, c);
        nk_fill_rect(cv, nk_rect(cx + s * 0.25f, cy - s, s * 0.55f, 2.0f * s), 1.0f, c);
    } else {
        nk_fill_triangle(cv, cx - s * 0.6f, cy - s, cx - s * 0.6f, cy + s, cx + s, cy, c);
    }
}

/* Picture-in-picture: the window shrinks to a corner of the screen and
 * stays above the others; again (or Escape) puts it back. */
static void
toggle_pip(void *user)
{
    SDL_Window *win = reaktor_sdl_window(ui.app);

    (void)user;
    if (!ui.pip) {
        SDL_Rect area;
        int w = 480, h = 270;

        if (fullscreen())
            SDL_SetWindowFullscreen(win, false);
        SDL_GetWindowPosition(win, &ui.pip_x, &ui.pip_y);
        SDL_GetWindowSize(win, &ui.pip_w, &ui.pip_h);
        if (ui.tex_w > 0 && ui.tex_h > 0)
            h = w * ui.tex_h / ui.tex_w;
        SDL_SetWindowAlwaysOnTop(win, true);
        SDL_SetWindowSize(win, w, h);
        if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(win), &area))
            SDL_SetWindowPosition(win, area.x + area.w - w - 24, area.y + area.h - h - 24);
    } else {
        SDL_SetWindowAlwaysOnTop(win, false);
        SDL_SetWindowSize(win, ui.pip_w, ui.pip_h);
        SDL_SetWindowPosition(win, ui.pip_x, ui.pip_y);
    }
    ui.pip = !ui.pip;
    touch();
}

static void
apply_volume(void)
{
    if (ui.player)
        papaya_player_set_volume(ui.player, ui.muted ? 0.0f : ui.volume);
}

static void
toggle_mute(void *user)
{
    (void)user;
    ui.muted = !ui.muted;
    if (!ui.muted && ui.volume <= 0.0f)
        ui.volume = 1.0f;
    apply_volume();
    touch();
}

static void toggle_settings(void *user);
static void toggle_playlist(void *user);
static float panel_bottom(float h);
static struct reaktor_rect popup_above(struct reaktor_rect button, float pw, float ph, float w, float h);

/* The control bar, over the bottom of the video. */
static void
draw_bar(struct reaktor_context *ctx, float w, float h, int64_t duration)
{
    char now[32], total[32], times[80];
    int playing = ui.player && !papaya_player_paused(ui.player);
    float before = ui.seek_value, volume_before = ui.volume;
    /* Minimal: across the bottom edge. Accent: a rounded, outlined bar
     * floating above it (its gradients wait on Reaktor's CSS). */
    float inset = ui.accent_style ? ACCENT_INSET : 0.0f, x = inset, bw = w - 2.0f * inset;
    float y = h - BAR_H - inset;
    const char *sound = ui.muted || ui.volume <= 0.0f ? "volume-mute"
                        : ui.volume < 0.5f            ? "volume-low"
                                                      : "volume-high";

    format_time(now, sizeof now, ui.player ? papaya_player_position(ui.player) : 0);
    if (duration > 0)
        format_time(total, sizeof total, duration);
    else
        snprintf(total, sizeof total, "--:--");
    snprintf(times, sizeof times, "%s / %s", now, total);

    if (ui.accent_style) {
        struct reaktor_rect box = reaktor_rect(x, y, bw, BAR_H);

        reaktor_fill_rect(reaktor_window_get_canvas(ctx), box, 14.0f, reaktor_rgba(15, 13, 17, 215));
        nk_stroke_rect(reaktor_window_get_canvas(ctx), box, 14.0f, 1.0f, reaktor_rgba(255, 255, 255, 46));
    } else {
        reaktor_fill_rect(reaktor_window_get_canvas(ctx), reaktor_rect(0, y, w, BAR_H), 0,
                          reaktor_rgba(0, 0, 0, 170));
    }
    /* The seek bar across the whole width, the buttons under it. */
    REAKTOR_ROW(.w = bw - 2.0f * SEEK_MARGIN, .h = SEEK_H, .ml = x + SEEK_MARGIN, .mt = y + 4.0f) {
        struct reaktor_rect track;

        if (reaktor_box_rect(&track))
            click_to_seek(ctx, track, duration);
        reaktor_slider(&(reaktor_slider_spec){
            .name = "seek", .value = &ui.seek_value, .lo = 0.0f,
            .hi = duration > 0 ? (float)duration / 1e6f : 1.0f, .step = 0.1f,
            .box = { .h = SEEK_H, .flags = REAKTOR_LAY_FILL_X } });
    }
    if (ui.player && duration > 0 && ui.seek_value != before) {
        papaya_player_seek(ui.player, (int64_t)(ui.seek_value * 1e6f));
        touch();
    }
    REAKTOR_ROW(.w = bw - 16.0f, .h = 36.0f, .ml = x + 8.0f, .mt = y + 4.0f + SEEK_H, .gap = 6.0f) {
        /* the one filled button: a papaya pill */
        reaktor_button(&(reaktor_button_spec){
            .icon = playing ? "pause" : "play", .name = "play", .tooltip = playing ? "Pause" : "Play",
            .keys = "Space", .disabled = !ui.player, .style = ".pill", .paint = paint_play,
            .paint_user = &playing,
            .box = { .w = 48.0f, .h = 30.0f, .mt = 3.0f, .mr = 4.0f }, .on_press = { toggle_pause, NULL } });
        reaktor_button(&(reaktor_button_spec){
            .icon = "play-skip-back", .name = "prev", .tooltip = "Previous file", .keys = "P",
            .disabled = ui.list.current <= 0, .flat = 1, .box = { .w = 36.0f, .h = 36.0f },
            .on_press = { step_file, &step_prev } });
        reaktor_button(&(reaktor_button_spec){
            .label = "-10", .name = "back10", .tooltip = "Back 10 s", .keys = "J", .disabled = !ui.player,
            .style = ".ghost", .fit_content = 1, .box = { .h = 36.0f, .mt = 2.5f },
            .on_press = { seek_button, &step_back10 } });
        reaktor_button(&(reaktor_button_spec){
            .label = "+10", .name = "fwd10", .tooltip = "Forward 10 s", .keys = "L", .disabled = !ui.player,
            .style = ".ghost", .fit_content = 1, .box = { .h = 36.0f, .mt = 2.5f },
            .on_press = { seek_button, &step_fwd10 } });
        reaktor_button(&(reaktor_button_spec){
            .icon = "play-skip-forward", .name = "next", .tooltip = "Next file", .keys = "N",
            .disabled = ui.list.current < 0 || ui.list.current + 1 >= ui.list.count, .flat = 1,
            .box = { .w = 36.0f, .h = 36.0f }, .on_press = { step_file, &step_next } });
        reaktor_label(&(reaktor_label_spec){
            .text = times, .name = "time", .color = "#f2f2f2", .box = { .h = 36.0f, .ml = 6.0f } });
        REAKTOR_ROW(.weight = 1.0f, .h = 36.0f, .flags = REAKTOR_LAY_FILL_X) {
        }
        REAKTOR_ROW(.w = 36.0f, .h = 36.0f) {
            reaktor_box_rect(&ui.list_button_rect);
            reaktor_button(&(reaktor_button_spec){
                .icon = "list-outline", .name = "playlist", .tooltip = "Playlist", .flat = 1,
                .box = { .w = 36.0f, .h = 36.0f }, .on_press = { toggle_playlist, NULL } });
        }
        reaktor_button(&(reaktor_button_spec){
            .icon = sound, .name = "mute", .tooltip = ui.muted ? "Unmute (M)" : "Mute (M)", .keys = "M",
            .flat = 1, .box = { .w = 36.0f, .h = 36.0f }, .on_press = { toggle_mute, NULL } });
        reaktor_slider(&(reaktor_slider_spec){
            .name = "volume", .value = &ui.volume, .lo = 0.0f, .hi = 1.0f, .step = 0.05f,
            .box = { .w = 90.0f, .h = 36.0f, .mr = 6.0f } });
        if (ui.player && papaya_player_video_stream(ui.player))
            reaktor_button(&(reaktor_button_spec){
                .icon = "copy-outline", .name = "pip",
                .tooltip = ui.pip ? "Back to the full window" : "Picture in picture", .flat = 1,
                .box = { .w = 30.0f, .h = 36.0f }, .on_press = { toggle_pip, NULL } });
        reaktor_button(&(reaktor_button_spec){
            .icon = fullscreen() ? "fullscreen-exit" : "fullscreen-enter", .name = "fullscreen",
            .tooltip = fullscreen() ? "Exit full screen (F)" : "Full screen (F)", .keys = "F", .flat = 1,
            /* narrower than the others: a flat icon's own margin is gap enough */
            .box = { .w = 30.0f, .h = 36.0f }, .on_press = { toggle_fullscreen, NULL } });
        REAKTOR_ROW(.w = 30.0f, .h = 36.0f) {
            reaktor_box_rect(&ui.cog_rect);
            reaktor_button(&(reaktor_button_spec){
                .icon = "options-outline", .name = "settings", .tooltip = "Settings", .flat = 1,
                .box = { .w = 30.0f, .h = 36.0f }, .on_press = { toggle_settings, NULL } });
        }
        if (ui.volume != volume_before) {
            ui.muted = ui.volume <= 0.0f;
            apply_volume();
            touch();
        }
    }
}

/* ---- settings and the window frame ------------------------------------- */

static void
toggle_settings(void *user)
{
    (void)user;
    ui.settings_open = !ui.settings_open;
    ui.playlist_open = 0;
    touch();
}

static void
toggle_playlist(void *user)
{
    (void)user;
    ui.playlist_open = !ui.playlist_open;
    ui.settings_open = 0;
    touch();
}

static void
window_minimize(void *user)
{
    (void)user;
    SDL_MinimizeWindow(reaktor_sdl_window(ui.app));
}

static void
window_maximize(void *user)
{
    SDL_Window *win = reaktor_sdl_window(ui.app);

    (void)user;
    if (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED)
        SDL_RestoreWindow(win);
    else
        SDL_MaximizeWindow(win);
}

static void
window_close(void *user)
{
    (void)user;
    reaktor_request_quit(ui.app);
}

static int
inside(struct reaktor_rect r, float x, float y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* With the custom title bar: edges resize, and the top strip drags the
 * window except over the top bar's controls. */
static SDL_HitTestResult SDLCALL
hit_test(SDL_Window *win, const SDL_Point *pt, void *data)
{
    const int edge = 6;
    int w = 0, h = 0, left, right, top, bottom;
    float x = (float)pt->x, y = (float)pt->y;

    (void)data;
    if (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED)
        top = left = right = bottom = 0;
    else {
        SDL_GetWindowSize(win, &w, &h);
        left = pt->x < edge;
        right = pt->x >= w - edge;
        top = pt->y < edge;
        bottom = pt->y >= h - edge;
    }
    if (top && left)
        return SDL_HITTEST_RESIZE_TOPLEFT;
    if (top && right)
        return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (bottom && left)
        return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (bottom && right)
        return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (top)
        return SDL_HITTEST_RESIZE_TOP;
    if (bottom)
        return SDL_HITTEST_RESIZE_BOTTOM;
    if (left)
        return SDL_HITTEST_RESIZE_LEFT;
    if (right)
        return SDL_HITTEST_RESIZE_RIGHT;
    if (y < TOP_H && (!ui.bars_shown || inside(ui.drag_rect, x, y)))
        return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
}

static void
set_titlebar(int custom)
{
    ui.custom_titlebar = custom;
    reaktor_set_borderless(ui.app, custom);
    reaktor_setting_set("titlebar", custom ? "custom" : "native");
    reaktor_settings_save();
}

static const char *const vis_styles[] = { "bars", "blocks", "wave", "off" };
static const char *const vis_colours[] = { "gradient", "accent", "hue" };

/* Which of names a setting holds, or fallback. */
static int
setting_index(const char *key, const char *const *names, int count, int fallback)
{
    const char *v = reaktor_setting(key);

    for (int i = 0; v && i < count; i++)
        if (strcmp(v, names[i]) == 0)
            return i;
    return fallback;
}

static void
load_settings(void)
{
    const char *t = reaktor_setting("titlebar"), *next = reaktor_setting("autoplay_next");

    ui.custom_titlebar = t && strcmp(t, "custom") == 0;
    ui.autoplay_next = !next || strcmp(next, "0") != 0;
    {
        const char *style = reaktor_setting("controls");

        ui.accent_style = style && strcmp(style, "accent") == 0;
    }
    ui.vis_style = setting_index("visualiser", vis_styles, 4, VIS_BARS);
    ui.vis_colour = setting_index("visualiser_colour", vis_colours, 3, VIS_GRADIENT);
    if (ui.custom_titlebar)
        reaktor_set_borderless(ui.app, 1);
}

#define SETTINGS_W 380.0f
#define SETTINGS_H 172.0f

static const char *const settings_tabs[] = { "Playback", "Visualiser", "Window" };

/* A setting's row: its name (and a line under it), then its control. */
static void
setting_label(const char *name, const char *text, const char *note)
{
    REAKTOR_COLUMN(.weight = 1.0f, .flags = REAKTOR_LAY_FILL_X) {
        reaktor_label(&(reaktor_label_spec){ .text = text, .name = name, .color = "#f2f2f2", .box = { .h = 20.0f } });
        if (note)
            reaktor_label(&(reaktor_label_spec){ .text = note, .color = "--text-muted", .box = { .h = 18.0f } });
    }
}

/* Two choices side by side; returns the one chosen. */
static int
setting_pair(const char *name_a, const char *a, const char *name_b, const char *b, int value)
{
    reaktor_bool on_a = value == 0, on_b = value != 0;

    if (reaktor_select(&(reaktor_select_spec){ .label = a, .name = name_a, .on = &on_a, .centered = 1,
                                              .box = { .w = 92.0f, .h = 32.0f } }))
        value = 0;
    if (reaktor_select(&(reaktor_select_spec){ .label = b, .name = name_b, .on = &on_b, .centered = 1,
                                              .box = { .w = 92.0f, .h = 32.0f } }))
        value = 1;
    return value;
}

/* One of count choices side by side; returns the one chosen. */
static int
setting_choice(const char *prefix, const char *const *labels, int count, int value, float width)
{
    for (int i = 0; i < count; i++) {
        reaktor_bool on = value == i;
        char name[48];

        snprintf(name, sizeof name, "%s-%d", prefix, i);
        if (reaktor_select(&(reaktor_select_spec){ .label = labels[i], .name = name, .on = &on, .centered = 1,
                                                  .box = { .w = width, .h = 32.0f } }))
            value = i;
    }
    return value;
}

/* Settings: a floating box above the options button. A click outside, the
 * button again or Escape closes it. */
static void
draw_settings(struct reaktor_context *ctx, float w, float h)
{
    const struct nk_mouse_button *left = &ctx->input.mouse.buttons[NK_BUTTON_LEFT];
    float mx = ctx->input.mouse.pos.x, my = ctx->input.mouse.pos.y;
    float pw = w - 16.0f < SETTINGS_W ? w - 16.0f : SETTINGS_W;
    static const char *const style_labels[] = { "Bars", "Blocks", "Wave", "Off" };
    static const char *const colour_labels[] = { "Gradient", "Accent", "Hue" };
    int titlebar = ui.custom_titlebar, style = ui.accent_style, vis = ui.vis_style, colour = ui.vis_colour;
    reaktor_bool next = ui.autoplay_next;

    if (left->clicked && left->down && !inside(ui.settings_rect, mx, my) && !inside(ui.cog_rect, mx, my)) {
        ui.settings_open = 0;
        return;
    }
    ui.settings_rect = popup_above(ui.cog_rect, pw, SETTINGS_H, w, h);
    reaktor_fill_rect(reaktor_window_get_canvas(ctx), ui.settings_rect, 10.0f, reaktor_rgba(24, 22, 27, 240));
    REAKTOR_COLUMN(.w = pw - 32.0f, .ml = ui.settings_rect.x + 16.0f, .mt = ui.settings_rect.y + 10.0f, .gap = 12.0f) {
        reaktor_tabs(&(reaktor_tabs_spec){
            .items = settings_tabs, .count = (int)(sizeof settings_tabs / sizeof settings_tabs[0]),
            .chosen = &ui.settings_tab, .name = "Settings", .box = { .flags = REAKTOR_LAY_FILL_X } });
        if (ui.settings_tab == 0) {
            REAKTOR_ROW(.h = 40.0f, .gap = 6.0f, .flags = REAKTOR_LAY_FILL_X) {
                setting_label("controls-heading", "Controls style", "Bottom edge, or floating");
                style = setting_pair("controls-minimal", "Minimal", "controls-accent", "Accent", style);
            }
            REAKTOR_ROW(.h = 40.0f, .gap = 6.0f, .flags = REAKTOR_LAY_FILL_X) {
                setting_label("next-heading", "Play the next file at the end", "Moves on through the folder");
                reaktor_check(&(reaktor_check_spec){ .label = "", .name = "autoplay-next", .on = &next,
                                                     .box = { .w = 32.0f, .h = 32.0f } });
            }
        } else if (ui.settings_tab == 1) {
            REAKTOR_ROW(.h = 40.0f, .gap = 6.0f, .flags = REAKTOR_LAY_FILL_X) {
                setting_label("vis-heading", "Style", NULL);
                vis = setting_choice("vis-style", style_labels, 4, vis, 64.0f);
            }
            REAKTOR_ROW(.h = 40.0f, .gap = 6.0f, .flags = REAKTOR_LAY_FILL_X) {
                setting_label("vis-colour-heading", "Colour", NULL);
                colour = setting_choice("vis-colour", colour_labels, 3, colour, 80.0f);
            }
        } else {
            REAKTOR_ROW(.h = 40.0f, .gap = 6.0f, .flags = REAKTOR_LAY_FILL_X) {
                setting_label("titlebar-heading", "Title bar", "The system's, or the top bar in its place");
                titlebar = setting_pair("titlebar-native", "Native", "titlebar-custom", "Custom", titlebar);
            }
        }
    }
    if (titlebar != ui.custom_titlebar)
        set_titlebar(titlebar);
    if (style != ui.accent_style) {
        ui.accent_style = style;
        reaktor_setting_set("controls", style ? "accent" : "minimal");
        reaktor_settings_save();
    }
    if (vis != ui.vis_style) {
        ui.vis_style = vis;
        reaktor_setting_set("visualiser", vis_styles[vis]);
        reaktor_settings_save();
    }
    if (colour != ui.vis_colour) {
        ui.vis_colour = colour;
        reaktor_setting_set("visualiser_colour", vis_colours[colour]);
        reaktor_settings_save();
    }
    if (next != ui.autoplay_next) {
        ui.autoplay_next = next;
        reaktor_setting_set("autoplay_next", next ? "1" : "0");
        reaktor_settings_save();
    }
    touch();
}

/* The top of the control bar. */
static float
panel_bottom(float h)
{
    return h - BAR_H - (ui.accent_style ? ACCENT_INSET : 0.0f);
}

/* A floating box pw x ph above a button of the control bar, its right edge
 * at the button's, kept inside the window and below the top bar. */
static struct reaktor_rect
popup_above(struct reaktor_rect button, float pw, float ph, float w, float h)
{
    float x = button.x + button.w - pw, y = panel_bottom(h) - 8.0f - ph;

    if (x > w - pw - 8.0f)
        x = w - pw - 8.0f;
    if (x < 8.0f)
        x = 8.0f;
    if (y < TOP_H + 8.0f)
        y = TOP_H + 8.0f;
    return reaktor_rect(x, y, pw, ph);
}

static const char *
playlist_cell(int row, int column, void *user)
{
    static char number[16];

    (void)user;
    if (column == 0) {
        snprintf(number, sizeof number, "%d", row + 1);
        return number;
    }
    return ui.list.files[row];
}

/* The folder's files in a floating box above the list button; a click plays
 * one. A click outside, the button again or Escape closes it. */
static void
draw_playlist(struct reaktor_context *ctx, float w, float h)
{
    static const char *const columns[] = { "#", "Name" };
    static const float widths[] = { 0.12f, 0.88f };
    const struct nk_mouse_button *left = &ctx->input.mouse.buttons[NK_BUTTON_LEFT];
    float mx = ctx->input.mouse.pos.x, my = ctx->input.mouse.pos.y;
    float pw = w - 16.0f < 380.0f ? w - 16.0f : 380.0f;
    float room = panel_bottom(h) - 8.0f - (TOP_H + 8.0f); /* the most height there is */
    float list_h = (float)(ui.list.count + 1) * 32.0f + 2.0f, ph;
    char heading[320];
    int chosen = ui.list.current;

    if (list_h > room - 56.0f)
        list_h = room - 56.0f;
    ph = (ui.list.count ? list_h : 0.0f) + 56.0f;

    if (left->clicked && left->down && !inside(ui.playlist_rect, mx, my) && !inside(ui.list_button_rect, mx, my)) {
        ui.playlist_open = 0;
        return;
    }
    ui.playlist_rect = popup_above(ui.list_button_rect, pw, ph, w, h);
    reaktor_fill_rect(reaktor_window_get_canvas(ctx), ui.playlist_rect, 10.0f, reaktor_rgba(24, 22, 27, 240));
    snprintf(heading, sizeof heading, "%s", ui.list.count ? ui.list.name : "No folder open");
    REAKTOR_COLUMN(.w = pw - 24.0f, .ml = ui.playlist_rect.x + 12.0f, .mt = ui.playlist_rect.y + 12.0f, .gap = 8.0f) {
        REAKTOR_ROW(.h = 24.0f, .flags = REAKTOR_LAY_FILL_X) {
            reaktor_label(&(reaktor_label_spec){ .text = heading, .name = "playlist-heading", .color = "#f2f2f2",
                                                 .box = { .weight = 1.0f, .h = 24.0f, .flags = REAKTOR_LAY_FILL_X } });
            if (ui.list.count) {
                char count[32];

                snprintf(count, sizeof count, "%d files", ui.list.count);
                reaktor_label(&(reaktor_label_spec){ .text = count, .color = "--text-muted", .box = { .h = 24.0f } });
            }
        }
        if (ui.list.count
            && reaktor_list(&(reaktor_list_spec){
                   .name = "Playlist", .count = ui.list.count, .columns = columns, .widths = widths,
                   .column_count = 2, .cell = playlist_cell, .selected = &chosen, .row_h = 32.0f,
                   /* as tall as its rows (and the header), up to the panel */
                   .box = { .h = list_h, .flags = REAKTOR_LAY_FILL_X } })
            && chosen != ui.list.current)
            play_index(chosen);
    }
    touch();
}

/* Band i's colour, at level v (0 at the foot of a bar, 1 at the top). */
static struct nk_color
vis_colour_at(int i, float v)
{
    if (ui.vis_colour == VIS_ACCENT)
        return reaktor_rgba(255, 122, 69, 255);
    if (ui.vis_colour == VIS_HUE) { /* round the colour wheel across the bands */
        float hue = (float)i / (float)VIS_BANDS * 6.0f, f = hue - (float)(int)hue;
        unsigned char up = (unsigned char)(60.0f + 195.0f * f), down = (unsigned char)(255.0f - 195.0f * f);

        switch ((int)hue) {
        case 0: return reaktor_rgba(255, up, 60, 255);
        case 1: return reaktor_rgba(down, 255, 60, 255);
        case 2: return reaktor_rgba(60, 255, up, 255);
        case 3: return reaktor_rgba(60, down, 255, 255);
        case 4: return reaktor_rgba(up, 60, 255, 255);
        default: return reaktor_rgba(255, 60, down, 255);
        }
    }
    /* guava at the foot to papaya at the top */
    return reaktor_rgba(255, (unsigned char)(61.0f + 61.0f * v), (unsigned char)(110.0f - 41.0f * v), 255);
}

/* The visualiser in r: the newest sound's levels, falling while paused. */
static void
draw_visualiser(struct reaktor_context *ctx, struct reaktor_rect r)
{
    static float samples[PAPAYA_SPECTRUM_WINDOW];
    struct nk_command_buffer *cv = reaktor_window_get_canvas(ctx);
    int64_t now = papaya_time_us();
    float dt = ui.vis_at ? (float)(now - ui.vis_at) / 1e6f : 0.0f, step = r.w / (float)VIS_BANDS, rest = 0.0f;
    int rate = 0;

    if (!ui.vis)
        ui.vis = papaya_spectrum_new(VIS_BANDS);
    if (!ui.vis)
        return;
    if (dt > 0.25f)
        dt = 0.25f;
    ui.vis_at = now;
    if (!papaya_player_paused(ui.player))
        rate = papaya_player_scope(ui.player, samples, PAPAYA_SPECTRUM_WINDOW);
    papaya_spectrum_update(ui.vis, rate ? samples : NULL, rate, dt, ui.vis_levels);
    for (int i = 0; i < VIS_BANDS; i++) {
        float v = ui.vis_levels[i], bh = v * r.h, bx = r.x + (float)i * step;

        rest += v;
        if (ui.vis_style == VIS_BARS && bh >= 1.0f) {
            struct reaktor_rect bar = reaktor_rect(bx + 1.0f, r.y + r.h - bh, step - 3.0f, bh);

            if (ui.vis_colour == VIS_GRADIENT) { /* corners: top left, top right, bottom right, bottom left */
                struct nk_color foot = vis_colour_at(i, 0.0f), tip = vis_colour_at(i, v);

                nk_fill_rect_multi_color(cv, bar, tip, tip, foot, foot);
            } else {
                reaktor_fill_rect(cv, bar, 2.0f, vis_colour_at(i, v));
            }
        } else if (ui.vis_style == VIS_BLOCKS) {
            int lit = (int)(v * 16.0f + 0.5f);
            float bk = r.h / 16.0f;

            for (int k = 0; k < 16; k++) {
                struct nk_color c = k < lit ? vis_colour_at(i, (float)k / 15.0f)
                                            : reaktor_rgba(255, 255, 255, 18); /* unlit blocks show faintly */

                reaktor_fill_rect(cv, reaktor_rect(bx + 1.0f, r.y + r.h - (float)(k + 1) * bk + 1.0f, step - 3.0f, bk - 2.0f),
                                  1.0f, c);
            }
        } else if (ui.vis_style == VIS_WAVE && i > 0) {
            float pv = ui.vis_levels[i - 1];

            reaktor_stroke_line(cv, bx - step / 2.0f, r.y + r.h - pv * r.h, bx + step / 2.0f, r.y + r.h - bh, 2.5f,
                                vis_colour_at(i, v));
        }
    }
    /* paused, but the bars are still falling: draw again */
    if (rest > 0.0f && papaya_player_paused(ui.player))
        reaktor_wake(ui.app);
}

/* A file with no picture: its cover art (or a papaya square), name and
 * visualiser where the video would be. */
static void
draw_music(struct reaktor_context *ctx, float w, float h)
{
    float art = h * 0.32f < 220.0f ? h * 0.32f : 220.0f, top = (h - BAR_H - art) / 2.0f, x = 48.0f;
    float tx = x + art + 24.0f, tw = w - tx - 48.0f;
    const char *name = ui.list.current >= 0 ? ui.list.files[ui.list.current] : ui.title;
    int vis = ui.vis_style != VIS_OFF && tw > 80.0f;

    if (top < TOP_H + 8.0f)
        top = TOP_H + 8.0f;
    if (ui.cover && ui.cover_w > 0 && ui.cover_h > 0) {
        /* the middle square of the picture */
        int side = ui.cover_w < ui.cover_h ? ui.cover_w : ui.cover_h;
        struct reaktor_image im = { 0 };

        im.handle.ptr = ui.cover;
        im.w = (unsigned short)ui.cover_w;
        im.h = (unsigned short)ui.cover_h;
        im.region[0] = (unsigned short)((ui.cover_w - side) / 2);
        im.region[1] = (unsigned short)((ui.cover_h - side) / 2);
        im.region[2] = (unsigned short)side;
        im.region[3] = (unsigned short)side;
        reaktor_draw_image(reaktor_window_get_canvas(ctx), reaktor_rect(x, top, art, art), &im,
                           reaktor_rgb(255, 255, 255));
    } else {
        reaktor_fill_rect(reaktor_window_get_canvas(ctx), reaktor_rect(x, top, art, art), 14.0f,
                          reaktor_rgba(255, 122, 69, 255));
        reaktor_fill_rect(reaktor_window_get_canvas(ctx),
                          reaktor_rect(x + art * 0.3f, top + art * 0.3f, art * 0.4f, art * 0.4f), art * 0.2f,
                          reaktor_rgba(255, 61, 110, 255));
    }
    /* the names centred beside the art, or at its top above the visualiser */
    REAKTOR_COLUMN(.w = tw, .ml = tx, .mt = vis ? top + 4.0f : top + art / 2.0f - 30.0f, .gap = 4.0f) {
        reaktor_label(&(reaktor_label_spec){ .text = ui.list.count ? ui.list.name : "", .color = "--text-muted",
                                             .box = { .h = 22.0f, .flags = REAKTOR_LAY_FILL_X } });
        reaktor_label(&(reaktor_label_spec){ .text = name, .name = "track-title", .color = "#f2f2f2",
                                             .style = ".track-title", .box = { .h = 34.0f, .flags = REAKTOR_LAY_FILL_X } });
    }
    if (vis)
        draw_visualiser(ctx, reaktor_rect(tx, top + 76.0f, tw < 720.0f ? tw : 720.0f, art - 76.0f));
}

/* The top bar: the folder (open file / folder, recent folders) and the file
 * within it. */
static void
draw_top(struct reaktor_context *ctx, float w)
{
    char folder[300], file[300];
    float file_w = w - 16.0f - 228.0f;
    int open = 0;

    snprintf(folder, sizeof folder, "%s", ui.list.name[0] ? ui.list.name : "Open");
    snprintf(file, sizeof file, "%s", ui.list.current >= 0 ? ui.list.files[ui.list.current] : ui.title);
    reaktor_fill_rect(reaktor_window_get_canvas(ctx), reaktor_rect(0, 0, w, TOP_H), 0,
                      reaktor_rgba(0, 0, 0, 120));
    REAKTOR_ROW(.w = w - 16.0f, .h = TOP_H, .ml = 8.0f, .gap = 8.0f) {
        REAKTOR_COMBO(.label = folder, .name = "folder", .body_h = 300.0f,
                      .box = { .w = 220.0f, .h = 36.0f, .mt = 8.0f }) {
            open = 1;
            reaktor_layout_row_dynamic(ctx, 28.0f, 1);
            if (reaktor_combo_item("Open file...", 0))
                ask_for_file(NULL);
            if (reaktor_combo_item("Open folder...", 0))
                ask_for_folder(NULL);
            for (int i = 0; i < ui.recent_count; i++)
                if (reaktor_combo_item(ui.recent[i], strcmp(ui.recent[i], ui.list.dir) == 0))
                    open_folder(ui.recent[i]);
        }
        if (ui.list.count > 0)
            REAKTOR_COMBO(.label = file, .name = "file", .body_h = 360.0f,
                          .box = { .w = file_w > 420.0f ? 420.0f : file_w, .h = 36.0f, .mt = 8.0f }) {
                open = 1;
                reaktor_layout_row_dynamic(ctx, 28.0f, 1);
                for (int i = 0; i < ui.list.count; i++)
                    if (reaktor_combo_item(ui.list.files[i], i == ui.list.current) && i != ui.list.current)
                        play_index(i);
            }
        REAKTOR_ROW(.weight = 1.0f, .h = TOP_H, .flags = REAKTOR_LAY_FILL_X) {
            if (!reaktor_box_rect(&ui.drag_rect))
                ui.drag_rect = reaktor_rect(0, 0, 0, 0);
        }
        if (ui.custom_titlebar) {
            int maximized = (SDL_GetWindowFlags(reaktor_sdl_window(ui.app)) & SDL_WINDOW_MAXIMIZED) != 0;

            reaktor_button(&(reaktor_button_spec){
                .icon = "remove", .name = "minimize", .tooltip = "Minimize", .flat = 1,
                .box = { .w = 36.0f, .h = 36.0f, .mt = 8.0f }, .on_press = { window_minimize, NULL } });
            reaktor_button(&(reaktor_button_spec){
                .icon = maximized ? "copy-outline" : "square-outline", .name = "maximize",
                .tooltip = maximized ? "Restore" : "Maximize", .flat = 1,
                /* a flat icon is 60% of the box height; the squares read big */
                .box = { .w = 36.0f, .h = 26.0f, .mt = 13.0f }, .on_press = { window_maximize, NULL } });
            reaktor_button(&(reaktor_button_spec){
                .icon = "close", .name = "close", .tooltip = "Close", .flat = 1,
                .box = { .w = 36.0f, .h = 36.0f, .mt = 8.0f }, .on_press = { window_close, NULL } });
        }
    }
    ui.menu_open = open;
    if (open)
        touch();
}

static void
page(App *app, struct reaktor_context *ctx, int win_w, int win_h)
{
    float w = (float)win_w, h = (float)win_h, mx, my;
    papaya_frame *f;
    int64_t duration = 0;
    int show_bar;

    /* Reaktor places boxes a frame late, so a new size needs a second frame;
     * when paused nothing else would draw it until the bar hides. */
    if (win_w != activity.win_w || win_h != activity.win_h) {
        activity.win_w = win_w;
        activity.win_h = win_h;
        reaktor_wake(app);
    }
    SDL_GetMouseState(&mx, &my);
    if (mx != activity.mouse_x || my != activity.mouse_y) {
        activity.mouse_x = mx;
        activity.mouse_y = my;
        touch();
    }
    if (ui.player) {
        f = papaya_player_video_frame(ui.player);
        if (f) {
            show_frame(f);
            papaya_frame_unref(&f);
        }
        duration = papaya_player_duration(ui.player);
        if (duration == PAPAYA_NOPTS)
            duration = 0;
        if (papaya_player_ended(ui.player) && !papaya_player_paused(ui.player)) {
            if (ui.autoplay_next && ui.list.current >= 0 && ui.list.current + 1 < ui.list.count)
                play_index(ui.list.current + 1); /* on to the next file */
            else
                set_paused(1);
        }
        ui.seek_value = (float)papaya_player_position(ui.player) / 1e6f;
    }
    /* While playing, the bar fades out of the way; paused or idle, it stays. */
    show_bar = !ui.player || papaya_player_paused(ui.player)
               || papaya_time_us() - activity.active_at < HIDE_AFTER_US;

    REAKTOR_FREE(.w = w, .h = h) {
        draw_video(ctx, w, h);
        if (ui.player && !papaya_player_video_stream(ui.player))
            draw_music(ctx, w, h);
        if (ui.error[0])
            reaktor_label(&(reaktor_label_spec){
                .text = ui.error,
                .name = "hint", .align = REAKTOR_TEXT_CENTERED, .color = "#dddddd",
                .box = { .w = w, .h = 30.0f, .mt = h / 2 - 15.0f } });
        ui.bars_shown = show_bar || ui.menu_open || ui.settings_open || ui.playlist_open;
        if (ui.bars_shown) {
            draw_top(ctx, w);
            draw_bar(ctx, w, h, duration);
        }
        if (ui.settings_open)
            draw_settings(ctx, w, h);
        if (ui.playlist_open)
            draw_playlist(ctx, w, h);
    }
}

/* ---- keys and lifetime -------------------------------------------------- */

static int
player_key(App *app, const SDL_Event *e)
{
    touch();
    if (reaktor_chord(e, 0, SDLK_F)) {
        toggle_fullscreen(NULL);
        return 1;
    }
    if (reaktor_chord(e, 0, SDLK_ESCAPE)) {
        if (ui.settings_open || ui.playlist_open)
            ui.settings_open = ui.playlist_open = 0;
        else if (ui.pip)
            toggle_pip(NULL);
        else if (fullscreen())
            toggle_fullscreen(NULL);
        else
            reaktor_request_quit(app);
        return 1;
    }
    if (reaktor_chord(e, 0, SDLK_LEFT)) {
        seek_by(-5000000);
        return 1;
    }
    if (reaktor_chord(e, 0, SDLK_RIGHT)) {
        seek_by(5000000);
        return 1;
    }
    if (reaktor_chord(e, 0, SDLK_O)) {
        ask_for_file(NULL);
        return 1;
    }
    if (reaktor_chord(e, 0, SDLK_M)) {
        toggle_mute(NULL);
        return 1;
    }
    if (reaktor_chord(e, 0, SDLK_J) || reaktor_chord(e, 0, SDLK_L)) {
        seek_by(reaktor_chord(e, 0, SDLK_J) ? -10000000 : 10000000);
        return 1;
    }
    if (reaktor_chord(e, 0, SDLK_P) || reaktor_chord(e, 0, SDLK_N)) {
        step_file(reaktor_chord(e, 0, SDLK_P) ? &step_prev : &step_next);
        return 1;
    }
    return 0;
}

static int
start(App *app, int argc, char **argv)
{
    ui.app = app;
    ui.list.current = -1;
    ui.volume = 1.0f;
    load_settings();
    ui.recent_count = recent_folders(ui.recent);
    for (int i = 1; i < argc; i++)
        if (argv[i][0] != '-') {
            open_file(argv[i]);
            break;
        }
    return 0;
}

static void
file_opened(App *app, const char *path)
{
    (void)app;
    open_file(path);
}

static void
stop(App *app)
{
    (void)app;
    close_file();
    playlist_free(&ui.list);
    free(ui.p010);
    free(ui.chroma420);
}

/* Our additions to Reaktor's stylesheet. */
/* Ionicons' arrow box icons say stroke="black", which Reaktor does not
 * recolor; these copies say "#000". */
static const char fullscreen_enter_svg[] =
    "<svg width=\"512\" height=\"512\" viewBox=\"0 0 512 512\" fill=\"none\" xmlns=\"http://www"
    ".w3.org/2000/svg\"><path d=\"M388.364 242.764V421.455C388.364 432.738 383.881 443.56 375.9"
    "02 451.539C367.924 459.518 357.102 464 345.818 464H90.5455C79.2617 464 68.4401 459.518 60."
    "4613 451.539C52.4825 443.56 48 432.738 48 421.455V166.182C48 154.898 52.4825 144.076 60.46"
    "13 136.098C68.4401 128.119 79.2617 123.636 90.5455 123.636H269.236\" stroke=\"#000\" strok"
    "e-width=\"32\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/><path d=\"M464 180.364L"
    "464 48L331.636 48\" stroke=\"#000\" stroke-width=\"32\" stroke-linecap=\"round\" stroke-li"
    "nejoin=\"round\"/><path d=\"M216 296L464 48\" stroke=\"#000\" stroke-width=\"32\" stroke-l"
    "inecap=\"round\" stroke-linejoin=\"round\"/></svg>";

static const char fullscreen_exit_svg[] =
    "<svg width=\"512\" height=\"512\" viewBox=\"0 0 512 512\" fill=\"none\" xmlns=\"http://www"
    ".w3.org/2000/svg\"><path d=\"M269.236 388.364L90.5454 388.364C79.2617 388.364 68.4401 383."
    "881 60.4612 375.902C52.4824 367.924 48 357.102 48 345.818L48 90.5454C48 79.2617 52.4824 68"
    ".4401 60.4613 60.4613C68.4401 52.4824 79.2617 48 90.5454 48L345.818 48C357.102 48 367.924 "
    "52.4825 375.902 60.4613C383.881 68.4401 388.364 79.2617 388.364 90.5455L388.364 269.236\" "
    "stroke=\"#000\" stroke-width=\"32\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/><p"
    "ath d=\"M331.636 464L464 464L464 331.636\" stroke=\"#000\" stroke-width=\"32\" stroke-line"
    "cap=\"round\" stroke-linejoin=\"round\"/><path d=\"M216 216L464 464\" stroke=\"#000\" stro"
    "ke-width=\"32\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/></svg>";

static const reaktor_asset papaya_assets[] = {
    { .name = "icons/fullscreen-enter.svg", .data = fullscreen_enter_svg, .size = sizeof fullscreen_enter_svg - 1 },
    { .name = "icons/fullscreen-exit.svg", .data = fullscreen_exit_svg, .size = sizeof fullscreen_exit_svg - 1 },
};

static const char papaya_css[] =
    /* Reaktor's accent (slider fills, focus, chosen items) comes from links. */
    "a:link { color: #ff7a45; }\n"
    ".pill { background-color: #ff7a45; color: #1b0e08; border-radius: 15px; }\n"
    ".pill:hover { background-color: #ff8f61; }\n"
    ".ghost { background-color: rgba(0, 0, 0, 0); color: #f2f2f2; border-radius: 0.5rem; padding: 8px 10px; }\n"
    ".ghost:hover { background-color: var(--button-hover); }\n"
    ".track-title { font-size: 1.6rem; font-weight: 600; }\n"
    /* the playlist: rows on the panel's own colour, the header quiet */
    ":root { --table: #232027; --table-bg: #18161b; --table-bg-alt: #1d1b21; }\n"
    "select { background-color: rgba(40, 40, 40, 0.55); color: #f2f2f2; }\n";

int
main(int argc, char **argv)
{
    /* Video needs a GPU renderer; Reaktor's default is software. A
     * --renderer given on the command line still wins. */
    char **args = (char **)malloc(sizeof(char *) * (size_t)(argc + 3));
    int n = 0, has_renderer = 0;

    if (!args)
        return 1;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--renderer") == 0)
            has_renderer = 1;
        args[n++] = argv[i];
    }
    if (!has_renderer) {
        args[n++] = "--renderer";
        args[n++] = "auto";
    }
    args[n] = NULL;
    return launchApp(n, args, &(reaktor_launch){
        .name        = "PapayaPlayer",
        .version     = PAPAYA_VERSION_STRING,
        .window      = { .w = 1280, .h = 720, .min_w = 480, .min_h = 270, .hit_test = hit_test },
        .theme       = REAKTOR_THEME_DARK,
        .page        = page,
        .start       = start,
        .key         = player_key,
        .stop        = stop,
        .file_opened = file_opened,
        .css         = { { .name = "papaya.css", .data = papaya_css, .size = sizeof papaya_css - 1 } },
        .icon_dirs   = { PAPAYA_ICON_DIR },
        .assets      = papaya_assets,
        .asset_count = (int)(sizeof papaya_assets / sizeof papaya_assets[0]) });
}
