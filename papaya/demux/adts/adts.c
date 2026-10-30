/* ADTS: raw AAC, each frame behind a 7-byte header (9 with a CRC) that says
 * the profile, sample rate, channels and frame length. The first header
 * becomes the AudioSpecificConfig the AAC decoder starts from, and packets
 * are the frames without their headers. Every frame's offset is indexed at
 * open (a header read per frame), so the length and seeking are exact. A
 * leading ID3v2 tag is skipped, and so is junk between frames: the index
 * resyncs on the next pair of matching headers.
 *
 * A frame may hold up to four raw data blocks (1024 samples each); without a
 * CRC they go to the decoder as one packet, which it decodes block by block.
 * With a CRC each block carries its own and the packet would need splitting:
 * not done, as no encoder in use writes that. Channel configuration 0 (the
 * layout in a PCE inside the stream) is not supported either. */
#include <stdlib.h>
#include <string.h>

#include "demux_internal.h"

typedef struct {
    int profile, rate_index, channels, rate;
    int header, size;   /* header bytes, whole frame bytes */
    int blocks;         /* raw data blocks in the frame */
} adts_header;

typedef struct {
    papaya_io *io;
    int64_t *offsets;   /* each frame's header */
    int64_t *starts;    /* each frame's first sample; [count] is the total */
    int64_t count, next;
    int rate;
    uint8_t config[2];  /* the AudioSpecificConfig */
} adts;

static const int rates[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000,
                               22050, 16000, 12000, 11025, 8000, 7350 };

static int
parse(const uint8_t *h, adts_header *o)
{
    if (h[0] != 0xFF || (h[1] & 0xF6) != 0xF0) /* sync, layer 0 */
        return -1;
    o->blocks = (h[6] & 3) + 1;
    /* with a CRC: the block positions, then the CRC */
    o->header = (h[1] & 1) ? 7 : 7 + 2 * (o->blocks - 1) + 2;
    o->profile = h[2] >> 6;
    o->rate_index = (h[2] >> 2) & 15;
    o->channels = (h[2] & 1) << 2 | h[3] >> 6;
    o->size = (h[3] & 3) << 11 | h[4] << 3 | h[5] >> 5;
    if (o->rate_index > 12 || o->size <= o->header)
        return -1;
    o->rate = rates[o->rate_index];
    return 0;
}

static int64_t
skip_id3(papaya_io *io)
{
    int64_t pos = 0;
    uint8_t b[10];

    while (papaya_io_read_at(io, pos, b, 10) == 0 && !memcmp(b, "ID3", 3))
        pos += 10 + ((int64_t)(b[6] & 127) << 21 | (b[7] & 127) << 14 | (b[8] & 127) << 7 | (b[9] & 127))
               + ((b[5] & 0x10) ? 10 : 0);
    return pos;
}

/* Whether a whole frame at pos matches first; strict: and so does the
 * header after it (or the file ends there), when resyncing on junk. */
static int
frame_at(papaya_io *io, int64_t pos, const adts_header *first, adts_header *h, int strict)
{
    uint8_t b[7], last;
    adts_header next;

    if (papaya_io_read_at(io, pos, b, 7) != 0 || parse(b, h) != 0 || h->rate_index != first->rate_index
        || h->profile != first->profile || papaya_io_read_at(io, pos + h->size - 1, &last, 1) != 0)
        return 0;
    if (!strict || papaya_io_read_at(io, pos + h->size, b, 7) != 0)
        return 1;
    return parse(b, &next) == 0 && next.rate_index == first->rate_index;
}

#define RESYNC_MAX ((int64_t)64 * 1024)

/* The next frame start after junk at pos, within RESYNC_MAX bytes, or -1. */
static int64_t
resync(papaya_io *io, int64_t pos, const adts_header *first)
{
    uint8_t w[4096];
    adts_header h;

    for (int64_t at = pos + 1; at < pos + RESYNC_MAX; at += (int64_t)sizeof w - 1) {
        size_t n = sizeof w;

        while (n > 1 && papaya_io_read_at(io, at, w, n) != 0)
            n /= 2; /* near the end of the file */
        if (n <= 1)
            return -1;
        for (size_t i = 0; i + 1 < n; i++)
            if (w[i] == 0xFF && (w[i + 1] & 0xF6) == 0xF0 && frame_at(io, at + (int64_t)i, first, &h, 1))
                return at + (int64_t)i;
        if (n < sizeof w)
            return -1;
    }
    return -1;
}

static int
adts_probe(const uint8_t *head, size_t size)
{
    adts_header h, next;

    if (size >= 10 && !memcmp(head, "ID3", 3))
        return 50; /* below MP3, which tries first */
    if (size < 7 || parse(head, &h) != 0)
        return 0;
    if ((size_t)h.size + 7 <= size)
        return parse(head + h.size, &next) == 0 && next.rate_index == h.rate_index ? 80 : 0;
    return 20;
}

static void *
adts_open(papaya_demuxer *d, papaya_io *io)
{
    int64_t pos = skip_id3(io), cap = 0;
    adts_header first, h;
    papaya_stream_info *s;
    uint8_t b[7];
    adts *c;

    if (papaya_io_read_at(io, pos, b, 7) != 0 || parse(b, &first) != 0 || first.channels == 0)
        return NULL;
    c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->io = io;
    c->rate = first.rate;
    /* The index: frames while their headers agree, over junk between them;
     * a cut-off last one ends it. */
    for (;;) {
        int64_t total = c->count ? c->starts[c->count] : 0;

        if (!frame_at(io, pos, &first, &h, 0)) {
            int64_t again;
            uint8_t probe;

            if (papaya_io_read_at(io, pos, &probe, 1) != 0)
                break;
            again = resync(io, pos, &first);
            if (again < 0)
                break;
            pos = again;
            continue;
        }
        if (c->count + 1 >= cap) {
            int64_t *grown_o, *grown_s;

            cap = cap ? cap * 2 : 4096;
            grown_o = realloc(c->offsets, sizeof *grown_o * (size_t)cap);
            if (grown_o)
                c->offsets = grown_o;
            grown_s = realloc(c->starts, sizeof *grown_s * (size_t)cap);
            if (grown_s)
                c->starts = grown_s;
            if (!grown_o || !grown_s)
                goto fail;
        }
        c->offsets[c->count] = pos;
        c->starts[c->count] = total;
        c->starts[++c->count] = total + (int64_t)1024 * h.blocks;
        pos += h.size;
    }
    if (c->count == 0)
        goto fail;

    /* AudioSpecificConfig: object type (profile + 1), rate index, channels. */
    c->config[0] = (uint8_t)((first.profile + 1) << 3 | first.rate_index >> 1);
    c->config[1] = (uint8_t)((first.rate_index & 1) << 7 | first.channels << 3);
    s = papaya_demux_add_stream(d);
    if (!s)
        goto fail;
    s->kind = PAPAYA_MEDIA_AUDIO;
    s->codec = PAPAYA_CODEC_AAC;
    s->codec_tag = "ADTS";
    s->sample_rate = first.rate;
    s->channels = first.channels;
    s->extradata = c->config;
    s->extradata_size = sizeof c->config;
    s->duration = c->starts[c->count] * 1000000 / c->rate;
    papaya_demux_set_duration(d, s->duration);
    return c;
fail:
    free(c->offsets);
    free(c->starts);
    free(c);
    return NULL;
}

static int
adts_read(void *ctx, papaya_packet **packet)
{
    adts *c = ctx;
    adts_header h;
    uint8_t b[7];
    papaya_packet *p;

    if (c->next >= c->count)
        return PAPAYA_DEMUX_EOF;
    if (papaya_io_read_at(c->io, c->offsets[c->next], b, 7) != 0 || parse(b, &h) != 0)
        return PAPAYA_DEMUX_ERROR;
    p = papaya_packet_new(NULL, (size_t)(h.size - h.header));
    if (!p)
        return PAPAYA_DEMUX_ERROR;
    if (papaya_io_read_at(c->io, c->offsets[c->next] + h.header, papaya_buffer_data(p->buf),
                          (size_t)(h.size - h.header)) != 0) {
        papaya_packet_free(&p);
        return PAPAYA_DEMUX_ERROR;
    }
    p->stream = 0;
    p->pts = p->dts = c->starts[c->next] * 1000000 / c->rate;
    p->duration = (c->starts[c->next + 1] - c->starts[c->next]) * 1000000 / c->rate;
    p->flags |= PAPAYA_PACKET_KEY;
    c->next++;
    *packet = p;
    return 0;
}

static int
adts_seek(void *ctx, int64_t pts)
{
    adts *c = ctx;
    int64_t sample = pts > 0 ? pts * c->rate / 1000000 : 0, lo = 0, hi = c->count;

    /* the last frame starting at or before the sample */
    while (lo + 1 < hi) {
        int64_t mid = (lo + hi) / 2;

        if (c->starts[mid] <= sample)
            lo = mid;
        else
            hi = mid;
    }
    c->next = sample >= c->starts[c->count] ? c->count : lo;
    return 0;
}

static void
adts_close(void *ctx)
{
    adts *c = ctx;

    free(c->offsets);
    free(c->starts);
    free(c);
}

const papaya_demux_backend papaya_demux_adts = {
    "adts", adts_probe, adts_open, adts_read, adts_seek, adts_close,
};
