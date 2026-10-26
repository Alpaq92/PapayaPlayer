#ifndef PAPAYA_IO_H
#define PAPAYA_IO_H

/* Byte sources for the demuxers: callbacks, so a file, memory or (later) an
 * HTTP stream look the same (TECH-STACK.md step 10). */

#include <stddef.h>
#include <stdint.h>

#include "papaya/export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct papaya_io {
    void *opaque;
    /* Reads up to size bytes; returns bytes read, 0 at end, < 0 on error. */
    int64_t (*read)(void *opaque, uint8_t *buf, size_t size);
    /* Moves to an absolute offset; returns 0 or < 0 on error. */
    int (*seek)(void *opaque, int64_t offset);
    /* Total size in bytes, or < 0 when unknown (live streams). */
    int64_t (*size)(void *opaque);
    void (*close)(void *opaque);
} papaya_io;

/* Opens a file for reading; path is UTF-8 on every platform. */
PAPAYA_EXPORT papaya_io *papaya_io_open_file(const char *path);
/* Reads from memory the caller keeps alive until the io is closed. */
PAPAYA_EXPORT papaya_io *papaya_io_open_memory(const uint8_t *data, size_t size);
PAPAYA_EXPORT void papaya_io_close(papaya_io **io);

/* Reads exactly size bytes at offset; returns 0, or < 0 if fewer were there. */
PAPAYA_EXPORT int papaya_io_read_at(papaya_io *io, int64_t offset, uint8_t *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif
