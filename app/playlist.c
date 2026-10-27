/* Folder playlists and the recent-folders list. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "playlist.h"
#include "reaktor/reaktor.h"

static const char *const media_ext[] = {
    "webm", "mkv", "mka", "ivf", "mp4", "m4v", "m4a", "mov", "wav", "mp3", "mp2", NULL,
};

static int
same_ignoring_case(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

int
playlist_is_media(const char *name)
{
    const char *dot = strrchr(name, '.');

    if (!dot)
        return 0;
    for (int i = 0; media_ext[i]; i++)
        if (same_ignoring_case(dot + 1, media_ext[i]))
            return 1;
    return 0;
}

static int
is_sep(char c)
{
    return c == '/' || c == '\\';
}

/* Natural order: runs of digits compare by value, the rest ignoring case. */
static int
natural_cmp(const void *a, const void *b)
{
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;

    while (*x && *y) {
        if (isdigit((unsigned char)*x) && isdigit((unsigned char)*y)) {
            const char *xs, *ys;
            size_t xn, yn;

            while (*x == '0')
                x++;
            while (*y == '0')
                y++;
            for (xs = x; isdigit((unsigned char)*x); x++)
                ;
            for (ys = y; isdigit((unsigned char)*y); y++)
                ;
            xn = (size_t)(x - xs);
            yn = (size_t)(y - ys);
            if (xn != yn)
                return xn < yn ? -1 : 1;
            {
                int c = strncmp(xs, ys, xn);

                if (c)
                    return c;
            }
            continue;
        }
        {
            int cx = tolower((unsigned char)*x), cy = tolower((unsigned char)*y);

            if (cx != cy)
                return cx < cy ? -1 : 1;
        }
        x++;
        y++;
    }
    return *x ? 1 : *y ? -1 : 0;
}

void
playlist_free(playlist *p)
{
    for (int i = 0; i < p->count; i++)
        free(p->files[i]);
    free((void *)p->files);
    p->files = NULL;
    p->count = 0;
    p->current = -1;
}

typedef struct {
    playlist *p;
    int cap;
} gather;

static SDL_EnumerationResult SDLCALL
add_entry(void *user, const char *dir, const char *name)
{
    gather *g = user;
    char full[2048];
    SDL_PathInfo info;

    if (!playlist_is_media(name))
        return SDL_ENUM_CONTINUE;
    snprintf(full, sizeof full, "%s%s", dir, name);
    if (!SDL_GetPathInfo(full, &info) || info.type != SDL_PATHTYPE_FILE)
        return SDL_ENUM_CONTINUE;
    if (g->p->count == g->cap) {
        int cap = g->cap ? g->cap * 2 : 64;
        char **grown = (char **)realloc((void *)g->p->files, sizeof(char *) * (size_t)cap);

        if (!grown)
            return SDL_ENUM_FAILURE;
        g->p->files = grown;
        g->cap = cap;
    }
    g->p->files[g->p->count] = SDL_strdup(name);
    if (!g->p->files[g->p->count])
        return SDL_ENUM_FAILURE;
    g->p->count++;
    return SDL_ENUM_CONTINUE;
}

int
playlist_load(playlist *p, const char *dir, const char *select)
{
    gather g = { p, 0 };
    size_t n;
    const char *base;

    playlist_free(p);
    snprintf(p->dir, sizeof p->dir, "%s", dir);
    n = strlen(p->dir);
    while (n > 1 && is_sep(p->dir[n - 1]) && !(n == 3 && p->dir[1] == ':'))
        p->dir[--n] = 0;
    for (base = p->dir + n; base > p->dir && !is_sep(base[-1]); base--)
        ;
    snprintf(p->name, sizeof p->name, "%s", *base ? base : p->dir);
    if (!SDL_EnumerateDirectory(p->dir, add_entry, &g))
        return -1;
    if (p->count > 1)
        qsort((void *)p->files, (size_t)p->count, sizeof(char *), natural_cmp);
    for (int i = 0; select && i < p->count; i++)
        if (strcmp(p->files[i], select) == 0)
            p->current = i;
    return 0;
}

int
playlist_load_file(playlist *p, const char *path)
{
    char dir[1024];
    const char *name = path + strlen(path);

    while (name > path && !is_sep(name[-1]))
        name--;
    if (name == path)
        snprintf(dir, sizeof dir, ".");
    else
        snprintf(dir, sizeof dir, "%.*s", (int)(name - path), path);
    return playlist_load(p, dir, name);
}

void
playlist_path(const playlist *p, int i, char *out, size_t size)
{
    size_t n = strlen(p->dir);

    snprintf(out, size, "%s%s%s", p->dir, n && is_sep(p->dir[n - 1]) ? "" : "/", p->files[i]);
}

/* ---- recent folders: one setting, entries separated by '|' -------------- */

int
recent_folders(char out[RECENT_MAX][1024])
{
    const char *s = reaktor_setting("recent_folders");
    int n = 0;

    while (s && *s && n < RECENT_MAX) {
        const char *end = strchr(s, '|');
        size_t len = end ? (size_t)(end - s) : strlen(s);

        if (len && len < 1024) {
            memcpy(out[n], s, len);
            out[n++][len] = 0;
        }
        s = end ? end + 1 : s + len;
    }
    return n;
}

void
recent_add(const char *dir)
{
    char old[RECENT_MAX][1024], joined[RECENT_MAX * 1025];
    int n = recent_folders(old);
    size_t at;

    if (strchr(dir, '|'))
        return;
    at = (size_t)snprintf(joined, sizeof joined, "%s", dir);
    for (int i = 0, kept = 1; i < n && kept < RECENT_MAX; i++)
        if (strcmp(old[i], dir) != 0) {
            at += (size_t)snprintf(joined + at, sizeof joined - at, "|%s", old[i]);
            kept++;
        }
    reaktor_setting_set("recent_folders", joined);
    reaktor_settings_save();
}
