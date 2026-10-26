#ifndef PAPAYA_APP_PLAYLIST_H
#define PAPAYA_APP_PLAYLIST_H

/* The playable files of one folder, in natural order ("2" before "10").
 * Subfolders are not included. */
typedef struct playlist {
    char dir[1024];   /* with no trailing separator */
    char name[256];   /* the folder's own name, for display */
    char **files;     /* names within dir */
    int count;
    int current;      /* -1 when nothing is playing from it */
} playlist;

/* Whether a file name has an extension the player opens. */
int playlist_is_media(const char *name);

/* Lists dir; current is set to the entry named select (or -1). */
int playlist_load(playlist *p, const char *dir, const char *select);

/* Loads the folder holding path and selects path in it. */
int playlist_load_file(playlist *p, const char *path);

void playlist_free(playlist *p);

/* Writes dir/files[i] into out. */
void playlist_path(const playlist *p, int i, char *out, size_t size);

/* The most recently opened folders, newest first, kept in the settings. */
#define RECENT_MAX 8
int recent_folders(char out[RECENT_MAX][1024]);
void recent_add(const char *dir);

#endif
