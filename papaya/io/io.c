#include "papaya/io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* ---- files -------------------------------------------------------------- */

typedef struct {
    FILE *f;
    int64_t size;
} file_io;

static int64_t
file_read(void *opaque, uint8_t *buf, size_t size)
{
    file_io *fi = opaque;
    size_t n = fread(buf, 1, size, fi->f);

    if (n == 0 && ferror(fi->f))
        return -1;
    return (int64_t)n;
}

static int
file_seek(void *opaque, int64_t offset)
{
    file_io *fi = opaque;

#ifdef _WIN32
    return _fseeki64(fi->f, offset, SEEK_SET) == 0 ? 0 : -1;
#else
    return fseeko(fi->f, (off_t)offset, SEEK_SET) == 0 ? 0 : -1;
#endif
}

static int64_t
file_size(void *opaque)
{
    return ((file_io *)opaque)->size;
}

static void
file_close(void *opaque)
{
    file_io *fi = opaque;

    fclose(fi->f);
    free(fi);
}

static FILE *
open_utf8(const char *path)
{
#ifdef _WIN32
    wchar_t *wide;
    FILE *f = NULL;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);

    if (n <= 0)
        return NULL;
    wide = malloc((size_t)n * sizeof *wide);
    if (!wide)
        return NULL;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, n);
    f = _wfopen(wide, L"rb");
    free(wide);
    return f;
#else
    return fopen(path, "rb");
#endif
}

papaya_io *
papaya_io_open_file(const char *path)
{
    papaya_io *io;
    file_io *fi;
    FILE *f = open_utf8(path);

    if (!f)
        return NULL;
    io = calloc(1, sizeof *io);
    fi = calloc(1, sizeof *fi);
    if (!io || !fi) {
        free(io);
        free(fi);
        fclose(f);
        return NULL;
    }
    fi->f = f;
    fi->size = -1;
    if (file_seek(fi, 0) == 0) {
#ifdef _WIN32
        if (_fseeki64(f, 0, SEEK_END) == 0)
            fi->size = _ftelli64(f);
#else
        if (fseeko(f, 0, SEEK_END) == 0)
            fi->size = (int64_t)ftello(f);
#endif
        file_seek(fi, 0);
    }
    io->opaque = fi;
    io->read = file_read;
    io->seek = file_seek;
    io->size = file_size;
    io->close = file_close;
    return io;
}

/* ---- memory ------------------------------------------------------------- */

typedef struct {
    const uint8_t *data;
    size_t size, pos;
} memory_io;

static int64_t
memory_read(void *opaque, uint8_t *buf, size_t size)
{
    memory_io *m = opaque;
    size_t left = m->size - m->pos;

    if (size > left)
        size = left;
    memcpy(buf, m->data + m->pos, size);
    m->pos += size;
    return (int64_t)size;
}

static int
memory_seek(void *opaque, int64_t offset)
{
    memory_io *m = opaque;

    if (offset < 0 || (uint64_t)offset > m->size)
        return -1;
    m->pos = (size_t)offset;
    return 0;
}

static int64_t
memory_size(void *opaque)
{
    return (int64_t)((memory_io *)opaque)->size;
}

papaya_io *
papaya_io_open_memory(const uint8_t *data, size_t size)
{
    papaya_io *io = calloc(1, sizeof *io);
    memory_io *m = calloc(1, sizeof *m);

    if (!io || !m) {
        free(io);
        free(m);
        return NULL;
    }
    m->data = data;
    m->size = size;
    io->opaque = m;
    io->read = memory_read;
    io->seek = memory_seek;
    io->size = memory_size;
    io->close = free;
    return io;
}

void
papaya_io_close(papaya_io **pio)
{
    papaya_io *io = *pio;

    *pio = NULL;
    if (!io)
        return;
    if (io->close)
        io->close(io->opaque);
    free(io);
}

int
papaya_io_read_at(papaya_io *io, int64_t offset, uint8_t *buf, size_t size)
{
    if (io->seek(io->opaque, offset) < 0)
        return -1;
    while (size) {
        int64_t n = io->read(io->opaque, buf, size);

        if (n <= 0)
            return -1;
        buf += n;
        size -= (size_t)n;
    }
    return 0;
}
