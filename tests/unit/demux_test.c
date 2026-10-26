#include <stdio.h>
#include <string.h>

#include "greatest.h"
#include "papaya/demux.h"

#define TESTDATA PAPAYA_LIBWEBM_TESTDATA "/"

static papaya_demuxer *
open_file(const char *name)
{
    char path[1024];

    snprintf(path, sizeof path, "%s%s", TESTDATA, name);
    return papaya_demux_open(papaya_io_open_file(path));
}

TEST
webm_streams(void)
{
    papaya_demuxer *d = open_file("bbb_480p_vp9_opus_1second.webm");
    const papaya_stream_info *v = NULL, *a = NULL;

    ASSERT(d != NULL);
    ASSERT_STR_EQ("webm", papaya_demux_name(d));
    ASSERT_EQ(2, papaya_demux_stream_count(d));
    for (int i = 0; i < 2; i++) {
        const papaya_stream_info *s = papaya_demux_stream(d, i);

        if (s->kind == PAPAYA_MEDIA_VIDEO)
            v = s;
        else
            a = s;
    }
    ASSERT(v && a);
    ASSERT_EQ(PAPAYA_CODEC_VP9, v->codec);
    ASSERT_EQ(854, v->width);
    ASSERT_EQ(480, v->height);
    ASSERT_EQ(PAPAYA_CODEC_OPUS, a->codec);
    ASSERT_EQ(48000, a->sample_rate);
    ASSERT_EQ(6, a->channels); /* the sample is 5.1 */
    ASSERT(a->extradata_size >= 19); /* OpusHead */
    ASSERT_MEM_EQ("OpusHead", a->extradata, 8);
    ASSERT(papaya_demux_duration(d) > 900000 && papaya_demux_duration(d) < 1100000);
    papaya_demux_close(&d);
    ASSERT(d == NULL);
    PASS();
}

TEST
webm_reads_every_packet_in_order(void)
{
    papaya_demuxer *d = open_file("bbb_480p_vp9_opus_1second.webm");
    papaya_packet *p;
    int64_t last[2] = { -1, -1 };
    int count[2] = { 0, 0 }, r, first_video_key = -1;

    ASSERT(d != NULL);
    while ((r = papaya_demux_read(d, &p)) == 0) {
        ASSERT(p->stream == 0 || p->stream == 1);
        ASSERT(p->size > 0);
        if (p->pts != PAPAYA_NOPTS) {
            if (papaya_demux_stream(d, p->stream)->kind == PAPAYA_MEDIA_AUDIO)
                ASSERT(p->pts >= last[p->stream]);
            last[p->stream] = p->pts;
        }
        if (papaya_demux_stream(d, p->stream)->kind == PAPAYA_MEDIA_VIDEO && first_video_key < 0)
            first_video_key = (p->flags & PAPAYA_PACKET_KEY) != 0;
        count[p->stream]++;
        papaya_packet_free(&p);
    }
    ASSERT_EQ(PAPAYA_DEMUX_EOF, r);
    ASSERT(count[0] > 10 && count[1] > 10);
    ASSERT_EQ(1, first_video_key);
    papaya_demux_close(&d);
    PASS();
}

TEST
webm_seek_lands_on_keyframe(void)
{
    papaya_demuxer *d = open_file("bbb_480p_vp9_opus_1second.webm");
    papaya_packet *p;
    int video = -1;

    ASSERT(d != NULL);
    for (int i = 0; i < papaya_demux_stream_count(d); i++)
        if (papaya_demux_stream(d, i)->kind == PAPAYA_MEDIA_VIDEO)
            video = i;
    ASSERT_EQ(0, papaya_demux_seek(d, 500000));
    for (;;) {
        ASSERT_EQ(0, papaya_demux_read(d, &p));
        if (p->stream == video)
            break;
        papaya_packet_free(&p);
    }
    ASSERT(p->flags & PAPAYA_PACKET_KEY);
    ASSERT(p->pts <= 500000);
    papaya_packet_free(&p);
    /* Seeking back to the start works too. */
    ASSERT_EQ(0, papaya_demux_seek(d, 0));
    ASSERT_EQ(0, papaya_demux_read(d, &p));
    ASSERT(p->pts >= 0 && p->pts < 50000); /* the file starts at 7 ms */
    papaya_packet_free(&p);
    papaya_demux_close(&d);
    PASS();
}

TEST
rejects_non_media(void)
{
    static const uint8_t junk[] = "this is not a media file";

    ASSERT(papaya_demux_open(papaya_io_open_memory(junk, sizeof junk)) == NULL);
    ASSERT(papaya_demux_open(papaya_io_open_memory(junk, 0)) == NULL);
    ASSERT(papaya_demux_open(NULL) == NULL);
    PASS();
}

/* Every libwebm sample, including the deliberately broken ones, opens or is
 * refused, and reads to the end, without crashing. */
TEST
survives_libwebm_testdata(void)
{
    static const char *const files[] = {
        "accurate_cluster_duration.webm", "accurate_cluster_duration_last_frame.webm",
        "accurate_cluster_duration_two_tracks.webm", "block_with_additional.webm",
        "chapters.webm", "colour.webm", "cues_before_clusters.webm", "discard_padding.webm",
        "estimate_duration.webm", "fixed_size_cluster_timecode.webm", "force_new_cluster.webm",
        "long_tag_string.webm", "matroska_doctype.mkv", "max_cluster_duration.webm",
        "max_cluster_size.webm", "metadata_block.webm", "output_cues.webm", "projection.webm",
        "segment_duration.webm", "segment_info.webm", "set_cues_track_number.webm",
        "set_pixelwidth_pixelheight.webm", "set_segment_duration.webm", "simple_block.webm",
        "test_stereo_left_right.webm", "tracks.webm", "webm_doctype.webm",
        "invalid/block_ends_beyond_cluster.mkv", "invalid/blockgroup_block_ends_beyond_blockgroup.mkv",
        "invalid/chapters_truncated_chapter_string.mkv",
        "invalid/chapters_truncated_chapter_string_2.mkv", "invalid/fixed_lacing_bad_lace_size.mkv",
        "invalid/invalid_vp9_bitstream-bug_1416.webm", "invalid/invalid_vp9_bitstream-bug_1417.webm",
        "invalid/primarychromaticity_fieldtoolarge.webm", "invalid/projection_float_overflow.webm",
    };
    int opened = 0;

    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        papaya_demuxer *d = open_file(files[i]);
        papaya_packet *p;
        int guard = 0;

        if (!d)
            continue;
        opened++;
        while (papaya_demux_read(d, &p) == 0 && guard++ < 100000)
            papaya_packet_free(&p);
        papaya_demux_seek(d, 0);
        papaya_demux_close(&d);
    }
    ASSERT(opened >= 20);
    PASS();
}

TEST
colour_metadata(void)
{
    papaya_demuxer *d = open_file("colour.webm");
    const papaya_stream_info *v;

    ASSERT(d != NULL);
    v = papaya_demux_stream(d, 0);
    ASSERT_EQ(PAPAYA_MEDIA_VIDEO, v->kind);
    ASSERT(v->color.matrix != 2 || v->color.primaries != 2 || v->color.transfer != 2);
    papaya_demux_close(&d);
    PASS();
}

SUITE(demux)
{
    RUN_TEST(webm_streams);
    RUN_TEST(webm_reads_every_packet_in_order);
    RUN_TEST(webm_seek_lands_on_keyframe);
    RUN_TEST(rejects_non_media);
    RUN_TEST(survives_libwebm_testdata);
    RUN_TEST(colour_metadata);
}

GREATEST_MAIN_DEFS();

int
main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(demux);
    GREATEST_MAIN_END();
}
