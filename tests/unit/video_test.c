#include <stdio.h>
#include <string.h>

#include "greatest.h"
#include "md5.h"
#include "papaya/decode.h"
#include "papaya/demux.h"

#define AV1_DATA PAPAYA_TESTDATA "/av1/"

/* Hashes a frame the way dav1d's md5 output does: every row of Y, then U and
 * V, width bytes per row (two per sample above 8 bits, little endian). */
static void
hash_frame(md5_ctx *m, const papaya_frame *f)
{
    int bytes = f->pix == PAPAYA_PIX_I420_10 ? 2 : 1;
    int cw = f->pix == PAPAYA_PIX_I444 ? f->width : (f->width + 1) / 2;
    int ch = f->pix == PAPAYA_PIX_I420 || f->pix == PAPAYA_PIX_I420_10 ? (f->height + 1) / 2 : f->height;

    for (int y = 0; y < f->height; y++)
        md5_update(m, f->planes[0] + (size_t)y * (size_t)f->strides[0], (size_t)f->width * (size_t)bytes);
    for (int p = 1; p <= 2; p++)
        for (int y = 0; y < ch; y++)
            md5_update(m, f->planes[p] + (size_t)y * (size_t)f->strides[p], (size_t)cw * (size_t)bytes);
}

/* Decodes every frame of file; returns the frame count, or -1. */
static int
decode_file(const char *path, char md5[33], papaya_pixel_format *pix)
{
    papaya_demuxer *dm = papaya_demux_open(papaya_io_open_file(path));
    papaya_frame_pool *pool = papaya_frame_pool_new();
    papaya_decoder *d;
    papaya_packet *p;
    papaya_frame *f;
    md5_ctx m;
    int frames = 0;

    if (!dm)
        return -1;
    d = papaya_decoder_open(papaya_demux_stream(dm, 0), pool);
    if (!d)
        return -1;
    md5_init(&m);
    while (papaya_demux_read(dm, &p) == 0) {
        if (papaya_decoder_send(d, p) != PAPAYA_DECODE_OK)
            return -1;
        papaya_packet_free(&p);
        while (papaya_decoder_receive(d, &f) == PAPAYA_DECODE_OK) {
            *pix = f->pix;
            hash_frame(&m, f);
            frames++;
            papaya_frame_unref(&f);
        }
    }
    papaya_decoder_send(d, NULL);
    while (papaya_decoder_receive(d, &f) == PAPAYA_DECODE_OK) {
        hash_frame(&m, f);
        frames++;
        papaya_frame_unref(&f);
    }
    md5_final(&m, md5);
    papaya_decoder_close(&d);
    papaya_frame_pool_release(&pool);
    papaya_demux_close(&dm);
    return frames;
}

TEST
md5_self_test(void)
{
    md5_ctx m;
    char hex[33];

    md5_init(&m);
    md5_update(&m, "The quick brown fox jumps over the lazy dog", 43);
    md5_final(&m, hex);
    ASSERT_STR_EQ("9e107d9d372bb6826bd81d3542a419d6", hex);
    PASS();
}

/* dav1d's own conformance vectors, with the MD5s from dav1d-test-data. */
TEST
av1_8bit_bit_exact(void)
{
    char md5[33];
    papaya_pixel_format pix = PAPAYA_PIX_NONE;
    int frames = decode_file(AV1_DATA "00000527.ivf", md5, &pix);

    ASSERT(frames > 0);
    ASSERT_EQ(PAPAYA_PIX_I420, pix);
    ASSERT_STR_EQ("7986b9d849114e52c5159aba52789d45", md5);
    PASS();
}

TEST
av1_10bit_bit_exact(void)
{
    char md5[33];
    papaya_pixel_format pix = PAPAYA_PIX_NONE;
    int frames = decode_file(AV1_DATA "00000671.ivf", md5, &pix);

    ASSERT(frames > 0);
    ASSERT_EQ(PAPAYA_PIX_I420_10, pix);
    ASSERT_STR_EQ("827a458a97061dfcdb8b05039ca1e531", md5);
    PASS();
}

SUITE(video)
{
    RUN_TEST(md5_self_test);
    RUN_TEST(av1_8bit_bit_exact);
    RUN_TEST(av1_10bit_bit_exact);
}

GREATEST_MAIN_DEFS();

int
main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(video);
    GREATEST_MAIN_END();
}
