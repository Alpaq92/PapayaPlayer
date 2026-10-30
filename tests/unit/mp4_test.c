#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "aac_encode.h"
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
patch32(buf *b, size_t at, uint32_t v)
{
    b->data[at] = (uint8_t)(v >> 24);
    b->data[at + 1] = (uint8_t)(v >> 16);
    b->data[at + 2] = (uint8_t)(v >> 8);
    b->data[at + 3] = (uint8_t)v;
}

/* The same track fragmented: an empty sample table and mvex in moov, then
 * fragments of FRAG frames, each a moof (tfhd, tfdt, trun) and its mdat. The
 * moov leaves the duration unknown, as live-made files do. */
#define FRAG 4

static void
build_fmp4(buf *b, const ivf_frames *v)
{
    size_t moov, trak, mdia, minf, stbl, stsd, entry, mvex, at = 0;

    put(b, "\0\0\0\x14" "ftypiso6\0\0\0\0iso6", 20);
    moov = open_box(b, "moov");
    {
        size_t mvhd = open_box(b, "mvhd");

        zeros(b, 12);
        be32(b, 30);
        be32(b, 0); /* duration: unknown */
        zeros(b, 80);
        close_box(b, mvhd);
    }
    trak = open_box(b, "trak");
    {
        size_t tkhd = open_box(b, "tkhd");

        zeros(b, 12);
        be32(b, 1); /* track_ID */
        zeros(b, 64);
        close_box(b, tkhd);
    }
    mdia = open_box(b, "mdia");
    {
        size_t mdhd = open_box(b, "mdhd"), hdlr;

        zeros(b, 12);
        be32(b, 30);
        be32(b, 0);
        be16(b, 0x55c4);
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
        const char *empty[] = { "stts", "stsc", "stco" };

        for (int i = 0; i < 3; i++) {
            size_t box = open_box(b, empty[i]);

            zeros(b, 8);
            close_box(b, box);
        }
        {
            size_t stsz = open_box(b, "stsz");

            zeros(b, 12);
            close_box(b, stsz);
        }
    }
    close_box(b, stbl);
    close_box(b, minf);
    close_box(b, mdia);
    close_box(b, trak);
    mvex = open_box(b, "mvex");
    {
        size_t trex = open_box(b, "trex");

        zeros(b, 4);
        be32(b, 1);       /* track_ID */
        be32(b, 1);       /* sample description */
        be32(b, 1);       /* duration: one frame */
        be32(b, 0);       /* size: in each trun */
        be32(b, 0x10000); /* flags: not a sync sample */
        close_box(b, trex);
    }
    close_box(b, mvex);
    close_box(b, moov);

    for (int first = 0; first < v->count; first += FRAG) {
        int n = v->count - first < FRAG ? v->count - first : FRAG;
        size_t moof = b->size, traf, data_offset_at, bytes = 0;

        open_box(b, "moof");
        {
            size_t mfhd = open_box(b, "mfhd");

            zeros(b, 4);
            be32(b, (uint32_t)(first / FRAG + 1));
            close_box(b, mfhd);
        }
        traf = open_box(b, "traf");
        {
            size_t tfhd = open_box(b, "tfhd"), tfdt, trun;

            be32(b, 0x20000); /* default-base-is-moof */
            be32(b, 1);
            close_box(b, tfhd);
            tfdt = open_box(b, "tfdt");
            be32(b, 0x01000000); /* version 1 */
            be32(b, 0);
            be32(b, (uint32_t)first);
            close_box(b, tfdt);
            trun = open_box(b, "trun");
            be32(b, 0x000205); /* data offset, first sample flags, sizes */
            be32(b, (uint32_t)n);
            data_offset_at = b->size;
            be32(b, 0);
            be32(b, first == 0 ? 0x02000000 : 0x10000); /* only frame 0 is a keyframe */
            for (int i = 0; i < n; i++)
                be32(b, v->sizes[first + i]);
            close_box(b, trun);
        }
        close_box(b, traf);
        close_box(b, moof);
        patch32(b, data_offset_at, (uint32_t)(b->size - moof + 8));
        {
            size_t mdat = open_box(b, "mdat");

            for (int i = 0; i < n; i++)
                bytes += v->sizes[first + i];
            put(b, v->frames + at, bytes);
            at += bytes;
            close_box(b, mdat);
        }
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

typedef void (*builder)(buf *b, const ivf_frames *v);

TEST
av1_in_mp4_bit_exact(builder build)
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
    build(&b, &v);
    d = papaya_demux_open(papaya_io_open_memory(b.data, b.size));
    ASSERT(d != NULL);
    /* The fragmented file's duration comes from its samples. */
    ASSERT_EQ((int64_t)v.count * 1000000 / 30, papaya_demux_duration(d));
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
truncated_mp4_is_safe(builder build)
{
    ivf_frames v;
    buf b = { 0 };

    ASSERT_EQ(0, read_ivf(AV1_IVF, &v));
    build(&b, &v);
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

/* ---- AAC gapless: an edit list, or iTunes' iTunSMPB tag ------------------ */

enum { AAC_RATE = 48000, AAC_N = 50000, AAC_DELAY = 2048 };

typedef struct {
    buf units;
    uint32_t sizes[256];
    int count;
} aac_units;

static void
add_unit(void *user, const unsigned char *data, int size)
{
    aac_units *u = user;

    if (u->count < 256) {
        put(&u->units, data, (size_t)size);
        u->sizes[u->count++] = (uint32_t)size;
    }
}

static void
desc(buf *b, int tag, size_t len)
{
    uint8_t h[2] = { (uint8_t)tag, (uint8_t)len };

    put(b, h, 2);
}

/* One mp4a track, every access unit in one chunk. itunes: the start and
 * length in iTunSMPB, else in an edit list. */
static void
build_m4a(buf *b, const aac_units *u, const unsigned char *asc, int asc_size, int itunes)
{
    size_t moov, trak, mdia, minf, stbl, stsd, entry, esds, stco_at;
    uint32_t total = (uint32_t)u->count * 1024;

    put(b, "\0\0\0\x14" "ftypM4A \0\0\0\0M4A ", 20);
    moov = open_box(b, "moov");
    {
        size_t mvhd = open_box(b, "mvhd");

        zeros(b, 12);
        be32(b, AAC_RATE);
        be32(b, total);
        zeros(b, 80);
        close_box(b, mvhd);
    }
    trak = open_box(b, "trak");
    if (!itunes) {
        size_t edts = open_box(b, "edts"), elst = open_box(b, "elst");

        zeros(b, 4);
        be32(b, 1);
        be32(b, AAC_N);     /* duration, movie timescale */
        be32(b, AAC_DELAY); /* media time: after the priming */
        be32(b, 0x10000);
        close_box(b, elst);
        close_box(b, edts);
    }
    mdia = open_box(b, "mdia");
    {
        size_t mdhd = open_box(b, "mdhd"), hdlr;

        zeros(b, 12);
        be32(b, AAC_RATE);
        be32(b, total);
        be16(b, 0x55c4);
        zeros(b, 2);
        close_box(b, mdhd);
        hdlr = open_box(b, "hdlr");
        zeros(b, 8);
        put(b, "soun", 4);
        zeros(b, 13);
        close_box(b, hdlr);
    }
    minf = open_box(b, "minf");
    stbl = open_box(b, "stbl");
    stsd = open_box(b, "stsd");
    zeros(b, 4);
    be32(b, 1);
    entry = open_box(b, "mp4a");
    zeros(b, 6);
    be16(b, 1);
    zeros(b, 8);
    be16(b, 2);  /* channels */
    be16(b, 16); /* sample size */
    zeros(b, 4);
    be32(b, (uint32_t)AAC_RATE << 16);
    esds = open_box(b, "esds");
    zeros(b, 4);
    desc(b, 0x03, 3 + 2 + 13 + 2 + (size_t)asc_size);
    zeros(b, 3); /* ES_ID, flags */
    desc(b, 0x04, 13 + 2 + (size_t)asc_size);
    put(b, "\x40\x15", 2); /* MPEG-4 audio, audio stream */
    zeros(b, 11);
    desc(b, 0x05, (size_t)asc_size);
    put(b, asc, (size_t)asc_size);
    close_box(b, esds);
    close_box(b, entry);
    close_box(b, stsd);
    {
        size_t stts = open_box(b, "stts"), stsc, stsz, stco;

        zeros(b, 4);
        be32(b, 1);
        be32(b, (uint32_t)u->count);
        be32(b, 1024);
        close_box(b, stts);
        stsc = open_box(b, "stsc");
        zeros(b, 4);
        be32(b, 1);
        be32(b, 1);
        be32(b, (uint32_t)u->count);
        be32(b, 1);
        close_box(b, stsc);
        stsz = open_box(b, "stsz");
        zeros(b, 8);
        be32(b, (uint32_t)u->count);
        for (int i = 0; i < u->count; i++)
            be32(b, u->sizes[i]);
        close_box(b, stsz);
        stco = open_box(b, "stco");
        zeros(b, 4);
        be32(b, 1);
        stco_at = b->size;
        be32(b, 0);
        close_box(b, stco);
    }
    close_box(b, stbl);
    close_box(b, minf);
    close_box(b, mdia);
    close_box(b, trak);
    if (itunes) {
        size_t udta = open_box(b, "udta"), meta = open_box(b, "meta"), ilst, item, part;
        char smpb[120];

        zeros(b, 4);
        ilst = open_box(b, "ilst");
        item = open_box(b, "----");
        part = open_box(b, "mean");
        zeros(b, 4);
        put(b, "com.apple.iTunes", 16);
        close_box(b, part);
        part = open_box(b, "name");
        zeros(b, 4);
        put(b, "iTunSMPB", 8);
        close_box(b, part);
        part = open_box(b, "data");
        be32(b, 1); /* UTF-8 */
        zeros(b, 4);
        snprintf(smpb, sizeof smpb, " 00000000 %08X %08X %016X 00000000 00000000", AAC_DELAY,
                 total - AAC_DELAY - AAC_N, AAC_N);
        put(b, smpb, strlen(smpb));
        close_box(b, part);
        close_box(b, item);
        close_box(b, ilst);
        close_box(b, meta);
        close_box(b, udta);
    }
    close_box(b, moov);
    patch32(b, stco_at, (uint32_t)b->size + 8);
    {
        size_t mdat = open_box(b, "mdat");

        put(b, u->units.data, u->units.size);
        close_box(b, mdat);
    }
}

TEST
aac_gapless_in_m4a(int itunes)
{
    static short pcm[AAC_N * 2];
    unsigned char asc[64];
    int asc_size = 0;
    aac_units u = { { 0 }, { 0 }, 0 };
    buf b = { 0 };
    papaya_demuxer *d;
    papaya_frame_pool *pool;
    papaya_decoder *dec;
    papaya_packet *p;
    papaya_frame *f;
    long samples = 0;
    int64_t first = PAPAYA_NOPTS;

    for (int i = 0; i < AAC_N; i++)
        pcm[2 * i] = pcm[2 * i + 1] = (short)(8000 * sin(2 * 3.14159265358979 * 440 * i / AAC_RATE));
    ASSERT(aac_encode(2, 0, AAC_RATE, 2, 128000, pcm, AAC_N, add_unit, &u, asc, &asc_size) > 0);
    ASSERT(u.count < 256);
    build_m4a(&b, &u, asc, asc_size, itunes);
    d = papaya_demux_open(papaya_io_open_memory(b.data, b.size));
    ASSERT(d != NULL);
    ASSERT_EQ(PAPAYA_CODEC_AAC, papaya_demux_stream(d, 0)->codec);
    ASSERT_EQ((int64_t)AAC_N * 1000000 / AAC_RATE, papaya_demux_stream(d, 0)->gapless_end);
    pool = papaya_frame_pool_new();
    dec = papaya_decoder_open(papaya_demux_stream(d, 0), pool);
    ASSERT(dec != NULL);
    while (papaya_demux_read(d, &p) == 0) {
        papaya_decoder_send(dec, p);
        papaya_packet_free(&p);
        while (papaya_decoder_receive(dec, &f) == PAPAYA_DECODE_OK) {
            if (first == PAPAYA_NOPTS)
                first = f->pts;
            samples += f->samples;
            papaya_frame_unref(&f);
        }
    }
    papaya_decoder_send(dec, NULL);
    while (papaya_decoder_receive(dec, &f) == PAPAYA_DECODE_OK) {
        samples += f->samples;
        papaya_frame_unref(&f);
    }
    /* The priming is gone and the padding too: exactly the input. */
    ASSERT_EQ(0, (int)first);
    ASSERT_EQ((long)AAC_N, samples);
    papaya_decoder_close(&dec);
    papaya_frame_pool_release(&pool);
    papaya_demux_close(&d);
    free(b.data);
    free(u.units.data);
    PASS();
}

SUITE(mp4)
{
    RUN_TEST1(aac_gapless_in_m4a, 0);
    RUN_TEST1(aac_gapless_in_m4a, 1);
    RUN_TEST1(av1_in_mp4_bit_exact, build_mp4);
    RUN_TEST1(av1_in_mp4_bit_exact, build_fmp4);
    RUN_TEST1(truncated_mp4_is_safe, build_mp4);
    RUN_TEST1(truncated_mp4_is_safe, build_fmp4);
}

GREATEST_MAIN_DEFS();

int
main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(mp4);
    GREATEST_MAIN_END();
}
