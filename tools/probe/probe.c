/* papaya-probe: prints a file's streams and packet summary, and optionally
 * tests seeking. Usage: papaya-probe FILE [SEEK_SECONDS] */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "papaya/demux.h"

static void
print_time(const char *label, int64_t t)
{
    if (t == PAPAYA_NOPTS)
        printf("%s -", label);
    else
        printf("%s %.3f", label, (double)t / 1e6);
}

int
main(int argc, char **argv)
{
    papaya_demuxer *d;
    papaya_packet *p;
    int counts[64] = { 0 }, keys[64] = { 0 }, r;
    int64_t first[64], last[64];

    if (argc < 2) {
        fprintf(stderr, "usage: %s FILE [SEEK_SECONDS]\n", argv[0]);
        return 2;
    }
    d = papaya_demux_open(papaya_io_open_file(argv[1]));
    if (!d) {
        fprintf(stderr, "%s: not recognised\n", argv[1]);
        return 1;
    }
    printf("container %s", papaya_demux_name(d));
    print_time(", duration", papaya_demux_duration(d));
    printf(" s\n");
    for (int i = 0; i < papaya_demux_stream_count(d); i++) {
        const papaya_stream_info *s = papaya_demux_stream(d, i);

        first[i] = last[i] = PAPAYA_NOPTS;
        printf("  #%d %s %s (%s)", i, s->kind == PAPAYA_MEDIA_VIDEO ? "video" : "audio",
               papaya_codec_name(s->codec), s->codec_tag ? s->codec_tag : "?");
        if (s->kind == PAPAYA_MEDIA_VIDEO)
            printf(" %dx%d colour %u/%u/%u%s", s->width, s->height, s->color.primaries,
                   s->color.transfer, s->color.matrix, s->color.full_range ? " full" : "");
        else
            printf(" %d Hz %d ch", s->sample_rate, s->channels);
        printf(" extradata %zu", s->extradata_size);
        if (s->codec_delay != PAPAYA_NOPTS && s->codec_delay)
            printf(" delay %" PRId64 " us", s->codec_delay);
        if (s->language[0])
            printf(" [%s]", s->language);
        printf("\n");
    }
    if (argc > 2 && papaya_demux_seek(d, (int64_t)(atof(argv[2]) * 1e6)) != 0)
        printf("seek failed\n");
    while ((r = papaya_demux_read(d, &p)) == 0) {
        int s = p->stream;

        counts[s]++;
        if (p->flags & PAPAYA_PACKET_KEY)
            keys[s]++;
        if (p->pts != PAPAYA_NOPTS) {
            if (first[s] == PAPAYA_NOPTS)
                first[s] = p->pts;
            last[s] = p->pts;
        }
        papaya_packet_free(&p);
    }
    for (int i = 0; i < papaya_demux_stream_count(d); i++) {
        printf("  #%d %d packets, %d key", i, counts[i], keys[i]);
        print_time(", first", first[i]);
        print_time(", last", last[i]);
        printf("\n");
    }
    printf("%s\n", r == PAPAYA_DEMUX_EOF ? "end of file" : "read error");
    papaya_demux_close(&d);
    return r == PAPAYA_DEMUX_EOF ? 0 : 1;
}
