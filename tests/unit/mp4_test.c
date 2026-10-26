#include <stdlib.h>
#include <string.h>

#include "greatest.h"
#include "md5.h"
#include "papaya/decode.h"
#include "papaya/demux.h"

#define AV1_IVF PAPAYA_TESTDATA "/av1/00000527.ivf"

/* ---- a tiny MP4 writer, enough to wrap the IVF vector's frames --------- */

typedef struct {
    uint8_t *data;
    size_t size, cap;
} buf;

static void
put(buf *b, const void *p, size_t n)
{
    if (b->size + n > b->cap) {
        uint8_t *grown;

        b->cap = (b->size + n) * 2;
        grown = realloc(b->data, b->cap);
        if (!grown)
            abort();
        b->data = grown;
    }
    memcpy(b->data + b->size, p, n);
    b->size += n;
}

static void
be32(buf *b, uint32_t v)
{
    uint8_t x[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };

    put(b, x, 4);
}

static void
be16(buf *b, uint32_t v)
{
    uint8_t x[2] = { (uint8_t)(v >> 8), (uint8_t)v };

    put(b, x, 2);
}

static void zeros(buf *b, size_t n) { while (n--) put(b, "", 1); }

/* Opens a box; close_box patches its size. */
static size_t
open_box(buf *b, const char *type)
{
    size_t at = b->size;

    be32(b, 0);
    put(b, type, 4);
    return at;
}

static void
close_box(buf *b, size_t at)
{
    uint32_t n = (uint32_t)(b->size - at);

    b->data[at] = (uint8_t)(n >> 24);
    b->data[at + 1] = (uint8_t)(n >> 16);
    b->data[at + 2] = (uint8_t)(n >> 8);
    b->data[at + 3] = (uint8_t)n;
}

typedef struct {
    int count, width, height;
    uint32_t sizes[64];
    uint8_t *frames;
    size_t frames_size;
} ivf_frames;

static int
read_ivf(const char *path, ivf_frames *v)
{
    FILE *f = fopen(path, "rb");
    uint8_t h[32], fh[12];

    memset(v, 0, sizeof *v);
    if (!f)
        return -1;
    if (fread(h, 1, 32, f) != 32) {
        fclose(f);
        return -1;
    }
    v->width = h[12] | h[13] << 8;
    v->height = h[14] | h[15] << 8;
    while (v->count < 64 && fread(fh, 1, 12, f) == 12) {
        uint32_t n = (uint32_t)fh[0] | (uint32_t)fh[1] << 8 | (uint32_t)fh[2] << 16 | (uint32_t)fh[3] << 24;
        uint8_t *grown = realloc(v->frames, v->frames_size + n);

        if (!grown || fread(grown + v->frames_size, 1, n, f) != n) {
            free(grown ? grown : v->frames);
            fclose(f);
            return -1;
        }
        v->frames = grown;
        v->frames_size += n;
        v->sizes[v->count++] = n;
    }
    fclose(f);
    return 0;
}

/* ftyp, moov (one av01 track, 30 fps, an edit list starting at media time 0),
 * then mdat with every frame in one chunk. */
static void
build_mp4(buf *b, const ivf_frames *v)
{
    size_t moov, trak, mdia, minf, stbl, stsd, entry, edts, mdat_offset_at;

    put(b, "\0\0\0\x14" "ftypisom\0\0\0\0isom", 20);
    moov = open_box(b, "moov");
    {
        size_t mvhd = open_box(b, "mvhd");

        zeros(b, 12);
        be32(b, 30);                 /* timescale */
        be32(b, (uint32_t)v->count); /* duration */
        zeros(b, 80);
        close_box(b, mvhd);
    }
    trak = open_box(b, "trak");
    edts = open_box(b, "edts");
    {
        size_t elst = open_box(b, "elst");

        zeros(b, 4);
        be32(b, 1);
        be32(b, (uint32_t)v->count);
        be32(b, 0);       /* media time */
        be32(b, 0x10000); /* rate 1.0 */
        close_box(b, elst);
    }
    close_box(b, edts);
    mdia = open_box(b, "mdia");
    {
        size_t mdhd = open_box(b, "mdhd"), hdlr;

        zeros(b, 12);
        be32(b, 30);
        be32(b, (uint32_t)v->count);
        be16(b, 0x55c4); /* "und" */
        zeros(b, 2);
        close_box(b, mdhd);
        hdlr = open_box(b, "hdlr");
        zeros(b, 8);
        put(b, "vide", 4);
        zeros(b, 13);
        close_box(b, hdlr);
    }
    minf = open_box(b, "minf");
    stbl = open_box(b, "stbl");
    stsd = open_box(b, "stsd");
    zeros(b, 4);
    be32(b, 1);
    entry = open_box(b, "av01");
    zeros(b, 6);
    be16(b, 1);
    zeros(b, 16);
    be16(b, (uint32_t)v->width);
    be16(b, (uint32_t)v->height);
    zeros(b, 50);
    close_box(b, entry);
    close_box(b, stsd);
    {
        size_t stts = open_box(b, "stts"), stsc, stsz, stco, stss;

        zeros(b, 4);
        be32(b, 1);
        be32(b, (uint32_t)v->count);
        be32(b, 1);
        close_box(b, stts);
        stsc = open_box(b, "stsc");
        zeros(b, 4);
        be32(b, 1);
        be32(b, 1);
        be32(b, (uint32_t)v->count);
        be32(b, 1);
        close_box(b, stsc);
        stsz = open_box(b, "stsz");
        zeros(b, 8);
        be32(b, (uint32_t)v->count);
        for (int i = 0; i < v->count; i++)
            be32(b, v->sizes[i]);
        close_box(b, stsz);
        stco = open_box(b, "stco");
        zeros(b, 4);
        be32(b, 1);
        mdat_offset_at = b->size;
        be32(b, 0); /* patched below */
        close_box(b, stco);
        stss = open_box(b, "stss");
        zeros(b, 4);
        be32(b, 1);
        be32(b, 1);
        close_box(b, stss);
    }
    close_box(b, stbl);
    close_box(b, minf);
    close_box(b, mdia);
    close_box(b, trak);
    close_box(b, moov);
    {
        uint32_t data_at = (uint32_t)b->size + 8;
        size_t mdat;

        b->data[mdat_offset_at] = (uint8_t)(data_at >> 24);
        b->data[mdat_offset_at + 1] = (uint8_t)(data_at >> 16);
        b->data[mdat_offset_at + 2] = (uint8_t)(data_at >> 8);
        b->data[mdat_offset_at + 3] = (uint8_t)data_at;
        mdat = open_box(b, "mdat");
        put(b, v->frames, v->frames_size);
        close_box(b, mdat);
    }
}

static void
hash_frame(md5_ctx *m, const papaya_frame *f)
{
    int cw = (f->width + 1) / 2, ch = (f->height + 1) / 2;

    for (int y = 0; y < f->height; y++)
        md5_update(m, f->planes[0] + (size_t)y * (size_t)f->strides[0], (size_t)f->width);
    for (int p = 1; p <= 2; p++)
        for (int y = 0; y < ch; y++)
            md5_update(m, f->planes[p] + (size_t)y * (size_t)f->strides[p], (size_t)cw);
}

TEST
av1_in_mp4_bit_exact(void)
{
    ivf_frames v;
    buf b = { 0 };
    papaya_demuxer *d;
    papaya_frame_pool *pool;
    papaya_decoder *dec;
    papaya_packet *p;
    papaya_frame *f;
    md5_ctx m;
    char hex[33];
    int packets = 0, frames = 0;
    int64_t last_pts = -1;

    ASSERT_EQ(0, read_ivf(AV1_IVF, &v));
    build_mp4(&b, &v);
    d = papaya_demux_open(papaya_io_open_memory(b.data, b.size));
    ASSERT(d != NULL);
    ASSERT_STR_EQ("mp4", papaya_demux_name(d));
    ASSERT_EQ(1, papaya_demux_stream_count(d));
    ASSERT_EQ(PAPAYA_CODEC_AV1, papaya_demux_stream(d, 0)->codec);
    ASSERT_EQ(v.width, papaya_demux_stream(d, 0)->width);
    pool = papaya_frame_pool_new();
    dec = papaya_decoder_open(papaya_demux_stream(d, 0), pool);
    ASSERT(dec != NULL);
    md5_init(&m);
    while (papaya_demux_read(d, &p) == 0) {
        ASSERT(p->pts > last_pts);
        ASSERT_EQ(packets == 0, (p->flags & PAPAYA_PACKET_KEY) != 0);
        last_pts = p->pts;
        packets++;
        papaya_decoder_send(dec, p);
        papaya_packet_free(&p);
        while (papaya_decoder_receive(dec, &f) == PAPAYA_DECODE_OK) {
            hash_frame(&m, f);
            frames++;
            papaya_frame_unref(&f);
        }
    }
    papaya_decoder_send(dec, NULL);
    while (papaya_decoder_receive(dec, &f) == PAPAYA_DECODE_OK) {
        hash_frame(&m, f);
        frames++;
        papaya_frame_unref(&f);
    }
    md5_final(&m, hex);
    ASSERT(packets > 1);
    ASSERT_EQ(v.count, packets);
    ASSERT_EQ(v.count, frames);
    ASSERT_EQ(1000000 / 30, (int)(last_pts / (packets - 1))); /* 30 fps timing */
    /* The same reference MD5 as the IVF file: the MP4 path is lossless. */
    ASSERT_STR_EQ("7986b9d849114e52c5159aba52789d45", hex);
    ASSERT_EQ(0, papaya_demux_seek(d, 400000));
    ASSERT_EQ(0, papaya_demux_read(d, &p));
    ASSERT_EQ(0, (int)p->pts); /* the only keyframe is the first */
    papaya_packet_free(&p);
    papaya_decoder_close(&dec);
    papaya_frame_pool_release(&pool);
    papaya_demux_close(&d);
    free(b.data);
    free(v.frames);
    PASS();
}

TEST
truncated_mp4_is_safe(void)
{
    ivf_frames v;
    buf b = { 0 };

    ASSERT_EQ(0, read_ivf(AV1_IVF, &v));
    build_mp4(&b, &v);
    /* Every cut of the file opens or is refused, and reads end, without crashing. */
    for (size_t cut = 8; cut < b.size; cut += 37) {
        papaya_demuxer *d = papaya_demux_open(papaya_io_open_memory(b.data, cut));
        papaya_packet *p;

        if (!d)
            continue;
        while (papaya_demux_read(d, &p) == 0)
            papaya_packet_free(&p);
        papaya_demux_close(&d);
    }
    free(b.data);
    free(v.frames);
    PASS();
}

SUITE(mp4)
{
    RUN_TEST(av1_in_mp4_bit_exact);
    RUN_TEST(truncated_mp4_is_safe);
}

GREATEST_MAIN_DEFS();

int
main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(mp4);
    GREATEST_MAIN_END();
}
