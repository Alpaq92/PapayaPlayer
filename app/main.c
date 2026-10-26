/* PapayaPlayer's window: the video scaled to fit, a top bar choosing the
 * folder and the file in it, and a control bar with play / pause, -10 / +10,
 * previous / next, a seek slider, the time and the volume. Both bars hide while
 * playing. The designed UI comes in phase 3.
 *
 * Keys: Space play / pause, Left / Right seek 5 s, J / L seek 10 s,
 * P / N previous / next file, M mute, F full screen, O open, Escape close
 * settings, leave full screen or quit.
 *
 * Settings (the cog): native or custom title bar, and whether playback moves
 * on to the next file. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "papaya/player.h"
#include "papaya/thread.h"
#include "papaya/version.h"
#include "playlist.h"
#include "reaktor/main.h"
#include "reaktor/reaktor.h"

#define BAR_H 48.0f
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
    int settings_open;
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

static void
show_frame(papaya_frame *f)
{
    if (f->pix != PAPAYA_PIX_I420 && f->pix != PAPAYA_PIX_I420_10) {
        snprintf(ui.error, sizeof ui.error, "4:2:2 / 4:4:4 video is not shown yet");
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

/* The control bar, over the bottom of the video. */
static void
draw_bar(struct reaktor_context *ctx, float w, float h, int64_t duration)
{
    char now[32], total[32], times[80];
    int playing = ui.player && !papaya_player_paused(ui.player);
    float before = ui.seek_value, volume_before = ui.volume, y = h - BAR_H;
    const char *sound = ui.muted || ui.volume <= 0.0f ? "volume-mute"
                        : ui.volume < 0.5f            ? "volume-low"
                                                      : "volume-high";

    format_time(now, sizeof now, ui.player ? papaya_player_position(ui.player) : 0);
    if (duration > 0)
        format_time(total, sizeof total, duration);
    else
        snprintf(total, sizeof total, "--:--");
    snprintf(times, sizeof times, "%s / %s", now, total);

    reaktor_fill_rect(reaktor_window_get_canvas(ctx), reaktor_rect(0, y, w, BAR_H), 0,
                      reaktor_rgba(0, 0, 0, 170));
    REAKTOR_ROW(.w = w - 16.0f, .h = BAR_H, .ml = 8.0f, .mt = y, .gap = 6.0f) {
        reaktor_button(&(reaktor_button_spec){
            .icon = playing ? "pause" : "play", .name = "play", .tooltip = playing ? "Pause" : "Play",
            .keys = "Space", .disabled = !ui.player, .flat = 1,
            .box = { .w = 36.0f, .h = 36.0f, .mt = 6.0f }, .on_press = { toggle_pause, NULL } });
        reaktor_button(&(reaktor_button_spec){
            .label = "-10", .name = "back10", .tooltip = "Back 10 s", .keys = "J", .disabled = !ui.player,
            .style = ".ghost", .fit_content = 1, .box = { .h = 36.0f, .mt = 8.5f },
            .on_press = { seek_button, &step_back10 } });
        reaktor_button(&(reaktor_button_spec){
            .label = "+10", .name = "fwd10", .tooltip = "Forward 10 s", .keys = "L", .disabled = !ui.player,
            .style = ".ghost", .fit_content = 1, .box = { .h = 36.0f, .mt = 8.5f },
            .on_press = { seek_button, &step_fwd10 } });
        reaktor_button(&(reaktor_button_spec){
            .icon = "play-skip-back", .name = "prev", .tooltip = "Previous file", .keys = "P",
            .disabled = ui.list.current <= 0, .flat = 1, .box = { .w = 36.0f, .h = 36.0f, .mt = 6.0f },
            .on_press = { step_file, &step_prev } });
        reaktor_button(&(reaktor_button_spec){
            .icon = "play-skip-forward", .name = "next", .tooltip = "Next file", .keys = "N",
            .disabled = ui.list.current < 0 || ui.list.current + 1 >= ui.list.count, .flat = 1,
            .box = { .w = 36.0f, .h = 36.0f, .mt = 6.0f }, .on_press = { step_file, &step_next } });
        REAKTOR_ROW(.weight = 1.0f, .h = 36.0f, .mt = 6.0f, .flags = REAKTOR_LAY_FILL_X) {
            struct reaktor_rect track;

            if (reaktor_box_rect(&track))
                click_to_seek(ctx, track, duration);
            reaktor_slider(&(reaktor_slider_spec){
                .name = "seek", .value = &ui.seek_value, .lo = 0.0f,
                .hi = duration > 0 ? (float)duration / 1e6f : 1.0f, .step = 0.1f,
                .box = { .h = 36.0f, .flags = REAKTOR_LAY_FILL_X } });
        }
        if (ui.player && duration > 0 && ui.seek_value != before) {
            papaya_player_seek(ui.player, (int64_t)(ui.seek_value * 1e6f));
            touch();
        }
        reaktor_label(&(reaktor_label_spec){
            .text = times, .name = "time", .color = "#f2f2f2", .align = REAKTOR_TEXT_CENTERED,
            .box = { .h = 36.0f, .mt = 6.0f, .ml = 6.0f } });
        reaktor_button(&(reaktor_button_spec){
            .icon = sound, .name = "mute", .tooltip = ui.muted ? "Unmute (M)" : "Mute (M)", .keys = "M",
            .flat = 1, .box = { .w = 36.0f, .h = 36.0f, .mt = 6.0f }, .on_press = { toggle_mute, NULL } });
        reaktor_slider(&(reaktor_slider_spec){
            .name = "volume", .value = &ui.volume, .lo = 0.0f, .hi = 1.0f, .step = 0.05f,
            .box = { .w = 90.0f, .h = 36.0f, .mt = 6.0f, .mr = 6.0f } });
        reaktor_button(&(reaktor_button_spec){
            .icon = fullscreen() ? "contract" : "expand", .name = "fullscreen",
            .tooltip = fullscreen() ? "Exit full screen (F)" : "Full screen (F)", .keys = "F", .flat = 1,
            .box = { .w = 36.0f, .h = 36.0f, .mt = 6.0f }, .on_press = { toggle_fullscreen, NULL } });
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

static void
load_settings(void)
{
    const char *t = reaktor_setting("titlebar"), *next = reaktor_setting("autoplay_next");

    ui.custom_titlebar = t && strcmp(t, "custom") == 0;
    ui.autoplay_next = !next || strcmp(next, "0") != 0;
    if (ui.custom_titlebar)
        reaktor_set_borderless(ui.app, 1);
}

#define SETTINGS_W 280.0f

/* The settings popup, under the cog; a click outside closes it. */
static void
draw_settings(struct reaktor_context *ctx, float w)
{
    const struct nk_mouse_button *left = &ctx->input.mouse.buttons[NK_BUTTON_LEFT];
    float x = ctx->input.mouse.pos.x, y = ctx->input.mouse.pos.y;
    int choice = ui.custom_titlebar;
    reaktor_bool next = ui.autoplay_next;

    if (left->clicked && left->down && !inside(ui.settings_rect, x, y) && !inside(ui.cog_rect, x, y)) {
        ui.settings_open = 0;
        return;
    }
    ui.settings_rect = reaktor_rect(w - SETTINGS_W - 8.0f, TOP_H, SETTINGS_W, 148.0f);
    reaktor_fill_rect(reaktor_window_get_canvas(ctx), ui.settings_rect, 10.0f, reaktor_rgba(24, 24, 24, 235));
    REAKTOR_COLUMN(.w = SETTINGS_W - 32.0f, .ml = ui.settings_rect.x + 16.0f, .mt = TOP_H + 12.0f, .gap = 4.0f) {
        reaktor_label(&(reaktor_label_spec){
            .text = "Title bar", .name = "titlebar-heading", .color = "#9a9a9a", .box = { .h = 24.0f } });
        REAKTOR_ROW(.h = 32.0f, .gap = 6.0f, .flags = REAKTOR_LAY_FILL_X) {
            reaktor_bool native = !choice, custom = choice != 0;

            if (reaktor_select(&(reaktor_select_spec){
                    .label = "Native", .name = "titlebar-native", .on = &native, .centered = 1,
                    .box = { .weight = 1.0f, .h = 32.0f, .flags = REAKTOR_LAY_FILL_X } }))
                choice = 0;
            if (reaktor_select(&(reaktor_select_spec){
                    .label = "Custom", .name = "titlebar-custom", .on = &custom, .centered = 1,
                    .box = { .weight = 1.0f, .h = 32.0f, .flags = REAKTOR_LAY_FILL_X } }))
                choice = 1;
        }
        reaktor_label(&(reaktor_label_spec){
            .text = "Playback", .name = "playback-heading", .color = "#9a9a9a", .box = { .h = 24.0f, .mt = 8.0f } });
        reaktor_check(&(reaktor_check_spec){
            .label = "Play the next file at the end", .name = "autoplay-next", .on = &next,
            .box = { .h = 28.0f } });
    }
    if (choice != ui.custom_titlebar)
        set_titlebar(choice);
    if (next != ui.autoplay_next) {
        ui.autoplay_next = next;
        reaktor_setting_set("autoplay_next", next ? "1" : "0");
        reaktor_settings_save();
    }
    touch();
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
        REAKTOR_ROW(.w = 36.0f, .h = 36.0f, .mt = 8.0f) {
            reaktor_box_rect(&ui.cog_rect);
            reaktor_button(&(reaktor_button_spec){
                .icon = "settings-outline", .name = "settings", .tooltip = "Settings", .flat = 1,
                .box = { .w = 36.0f, .h = 36.0f }, .on_press = { toggle_settings, NULL } });
        }
        if (ui.custom_titlebar) {
            int maximized = (SDL_GetWindowFlags(reaktor_sdl_window(ui.app)) & SDL_WINDOW_MAXIMIZED) != 0;

            reaktor_button(&(reaktor_button_spec){
                .icon = "remove", .name = "minimize", .tooltip = "Minimize", .flat = 1,
                .box = { .w = 36.0f, .h = 36.0f, .mt = 8.0f }, .on_press = { window_minimize, NULL } });
            reaktor_button(&(reaktor_button_spec){
                .icon = maximized ? "copy-outline" : "square-outline", .name = "maximize",
                .tooltip = maximized ? "Restore" : "Maximize", .flat = 1,
                .box = { .w = 36.0f, .h = 36.0f, .mt = 8.0f }, .on_press = { window_maximize, NULL } });
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

    (void)app;
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
        if (ui.error[0])
            reaktor_label(&(reaktor_label_spec){
                .text = ui.error,
                .name = "hint", .align = REAKTOR_TEXT_CENTERED, .color = "#dddddd",
                .box = { .w = w, .h = 30.0f, .mt = h / 2 - 15.0f } });
        ui.bars_shown = show_bar || ui.menu_open || ui.settings_open;
        if (ui.bars_shown) {
            draw_top(ctx, w);
            draw_bar(ctx, w, h, duration);
        }
        if (ui.settings_open)
            draw_settings(ctx, w);
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
        if (ui.settings_open)
            ui.settings_open = 0;
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
}

/* Our additions to Reaktor's stylesheet. */
static const char papaya_css[] =
    ".ghost { background-color: rgba(0, 0, 0, 0); color: #f2f2f2; border-radius: 0.5rem; padding: 8px 10px; }\n"
    ".ghost:hover { background-color: var(--button-hover); }\n";

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
        .icon_dirs   = { PAPAYA_ICON_DIR } });
}
