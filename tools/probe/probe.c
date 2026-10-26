/* papaya-probe: prints a file's streams and packet summary, and optionally
 * tests seeking or measures decoding speed.
 * Usage: papaya-probe FILE [SEEK_SECONDS]
 *        papaya-probe --decode FILE      decode every stream, report speed
 *        papaya-probe --play FILE        play in real time (no sound card needed),
 *                                        report frames shown, dropped and lateness */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "papaya/decode.h"
#include "papaya/demux.h"
#include "papaya/player.h"
#include "papaya/thread.h"

static void
print_time(const char *label, int64_t t)
{
    if (t == PAPAYA_NOPTS)
        printf("%s -", label);
    else
        printf("%s %.3f", label, (double)t / 1e6);
}

/* Decodes every stream as fast as possible and reports frames per second. */
static int
decode_all(const char *path)
{
    papaya_demuxer *d = papaya_demux_open(papaya_io_open_file(path));
    papaya_frame_pool *pool = papaya_frame_pool_new();
    papaya_decoder *dec[64] = { 0 };
    int frames[64] = { 0 }, n;
    int64_t start, media_end = 0;
    papaya_packet *p;
    papaya_frame *f;

    if (!d) {
        fprintf(stderr, "%s: not recognised\n", path);
        return 1;
    }
    n = papaya_demux_stream_count(d);
    for (int i = 0; i < n && i < 64; i++)
        dec[i] = papaya_decoder_open(papaya_demux_stream(d, i), pool);
    start = papaya_time_us();
    while (papaya_demux_read(d, &p) == 0) {
        int s = p->stream;

        if (s < 64 && dec[s]) {
            papaya_decoder_send(dec[s], p);
            while (papaya_decoder_receive(dec[s], &f) == PAPAYA_DECODE_OK) {
                if (f->pts != PAPAYA_NOPTS && f->pts > media_end)
                    media_end = f->pts;
                frames[s]++;
                papaya_frame_unref(&f);
            }
        }
        papaya_packet_free(&p);
    }
    for (int i = 0; i < n && i < 64; i++) {
        if (!dec[i])
            continue;
        papaya_decoder_send(dec[i], NULL);
        while (papaya_decoder_receive(dec[i], &f) == PAPAYA_DECODE_OK) {
            frames[i]++;
            papaya_frame_unref(&f);
        }
    }
    {
        double secs = (double)(papaya_time_us() - start) / 1e6;

        for (int i = 0; i < n && i < 64; i++)
            if (dec[i])
                printf("  #%d %s: %d frames\n", i, papaya_decoder_name(dec[i]), frames[i]);
        printf("decoded %.2f s of media in %.2f s (%.1fx real time)\n", (double)media_end / 1e6, secs,
               secs > 0 ? (double)media_end / 1e6 / secs : 0.0);
    }
    for (int i = 0; i < n && i < 64; i++)
        papaya_decoder_close(&dec[i]);
    papaya_frame_pool_release(&pool);
    papaya_demux_close(&d);
    return 0;
}

/* Plays the file through the player engine as a UI would, polling every 2 ms. */
static int
play(const char *path)
{
    papaya_player_config cfg = { .null_audio = 1 };
    papaya_player *p = papaya_player_open(path, &cfg);
    int shown = 0, out_of_order = 0;
    int64_t last = -1, worst = 0, start, limit;

    if (!p) {
        fprintf(stderr, "%s: cannot play\n", path);
        return 1;
    }
    limit = papaya_player_duration(p) == PAPAYA_NOPTS ? 60000000 : papaya_player_duration(p) + 5000000;
    papaya_player_set_paused(p, 0);
    start = papaya_time_us();
    while (!papaya_player_ended(p) && papaya_time_us() - start < limit) {
        papaya_frame *f = papaya_player_video_frame(p);

        if (f) {
            int64_t lag = papaya_player_position(p) - f->pts;

            if (shown > 0 && lag > worst)
                worst = lag;
            if (f->pts < last)
                out_of_order++;
            last = f->pts;
            shown++;
            papaya_frame_unref(&f);
        }
        papaya_sleep_us(2000);
    }
    printf("played %.2f s in %.2f s: %d frames shown, %d dropped, %d out of order, worst lag %.1f ms%s\n",
           (double)papaya_player_position(p) / 1e6, (double)(papaya_time_us() - start) / 1e6, shown,
           papaya_player_dropped(p), out_of_order, (double)worst / 1000.0,
           papaya_player_ended(p) ? "" : " (did not finish)");
    papaya_player_close(&p);
    return 0;
}

int
main(int argc, char **argv)
{
    papaya_demuxer *d;
    papaya_packet *p;
    int counts[64] = { 0 }, keys[64] = { 0 }, r;
    int64_t first[64], last[64];

    if (argc == 3 && strcmp(argv[1], "--decode") == 0)
        return decode_all(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--play") == 0)
        return play(argv[2]);
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
    for (int i = 0; i < 64; i++)
        first[i] = last[i] = PAPAYA_NOPTS;
    for (int i = 0; i < papaya_demux_stream_count(d); i++) {
        const papaya_stream_info *s = papaya_demux_stream(d, i);

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
    if (argc > 2 && papaya_demux_seek(d, (int64_t)(strtod(argv[2], NULL) * 1e6)) != 0)
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
