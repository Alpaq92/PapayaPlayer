#include <stdint.h>
#include <string.h>

#include "greatest.h"
#include "papaya/clock.h"
#include "papaya/media.h"
#include "papaya/queue.h"
#include "papaya/thread.h"
#include "papaya/version.h"

/* ---- version ------------------------------------------------------------ */

TEST
version_matches_header(void)
{
    ASSERT_STR_EQ(PAPAYA_VERSION_STRING, papaya_version());
    ASSERT_EQ(PAPAYA_VERSION_MAJOR * 10000 + PAPAYA_VERSION_MINOR * 100 + PAPAYA_VERSION_PATCH,
              papaya_version_number());
    PASS();
}

/* ---- buffers and packets ------------------------------------------------ */

static int wrapped_freed;

static void
count_free(void *opaque, uint8_t *data)
{
    (void)opaque;
    (void)data;
    wrapped_freed++;
}

TEST
buffer_alloc_is_aligned_and_padded(void)
{
    papaya_buffer *b = papaya_buffer_alloc(100);
    uint8_t *d;

    ASSERT(b != NULL);
    d = papaya_buffer_data(b);
    ASSERT_EQ(0u, (unsigned)((uintptr_t)d % 64));
    ASSERT_EQ(100u, (unsigned)papaya_buffer_size(b));
    for (int i = 0; i < 64; i++)
        ASSERT_EQ(0, d[100 + i]);
    papaya_buffer_unref(&b);
    ASSERT(b == NULL);
    PASS();
}

TEST
buffer_wrap_frees_on_last_unref(void)
{
    static uint8_t mem[8];
    papaya_buffer *a, *b;

    wrapped_freed = 0;
    a = papaya_buffer_wrap(mem, sizeof mem, count_free, NULL);
    b = papaya_buffer_ref(a);
    papaya_buffer_unref(&a);
    ASSERT_EQ(0, wrapped_freed);
    papaya_buffer_unref(&b);
    ASSERT_EQ(1, wrapped_freed);
    PASS();
}

TEST
packet_copies_data(void)
{
    const uint8_t src[] = { 1, 2, 3, 4 };
    papaya_packet *p = papaya_packet_new(src, sizeof src);

    ASSERT(p != NULL);
    ASSERT_EQ(4u, (unsigned)p->size);
    ASSERT_MEM_EQ(src, p->data, sizeof src);
    ASSERT(p->pts == PAPAYA_NOPTS);
    papaya_packet_free(&p);
    ASSERT(p == NULL);
    PASS();
}

/* ---- frame pool --------------------------------------------------------- */

TEST
pool_video_planes(void)
{
    papaya_frame_pool *pool = papaya_frame_pool_new();
    papaya_frame *f = papaya_frame_get_video(pool, PAPAYA_PIX_I420, 641, 361);

    ASSERT(f != NULL);
    ASSERT_EQ(PAPAYA_MEDIA_VIDEO, f->kind);
    ASSERT(f->strides[0] >= 641 && f->strides[0] % 64 == 0);
    ASSERT(f->strides[1] >= 321);
    ASSERT(f->planes[1] >= f->planes[0] + (size_t)f->strides[0] * 361);
    ASSERT(f->planes[2] >= f->planes[1] + (size_t)f->strides[1] * 181);
    memset(f->planes[2], 0x80, (size_t)f->strides[2] * 181);
    papaya_frame_unref(&f);
    papaya_frame_pool_release(&pool);
    PASS();
}

TEST
pool_reuses_frames(void)
{
    papaya_frame_pool *pool = papaya_frame_pool_new();
    papaya_frame *a = papaya_frame_get_video(pool, PAPAYA_PIX_NV12, 64, 64);
    papaya_frame *first = a, *b;

    papaya_frame_unref(&a);
    b = papaya_frame_get_video(pool, PAPAYA_PIX_NV12, 32, 32);
    ASSERT(b == first);
    ASSERT_EQ(32, b->width);
    papaya_frame_unref(&b);
    papaya_frame_pool_release(&pool);
    PASS();
}

TEST
pool_outlives_release_while_frames_held(void)
{
    papaya_frame_pool *pool = papaya_frame_pool_new();
    papaya_frame *f = papaya_frame_get_audio(pool, PAPAYA_SAMPLE_F32, 2, 48000, 1024);
    papaya_frame *g;

    ASSERT(f != NULL);
    ASSERT_EQ(PAPAYA_MEDIA_AUDIO, f->kind);
    g = papaya_frame_ref(f);
    papaya_frame_pool_release(&pool);
    ((float *)f->audio[0])[2047] = 1.0f; /* still valid memory */
    papaya_frame_unref(&f);
    papaya_frame_unref(&g); /* last reference: destroys the pool */
    PASS();
}

TEST
pool_rejects_bad_sizes(void)
{
    papaya_frame_pool *pool = papaya_frame_pool_new();

    ASSERT(papaya_frame_get_video(pool, PAPAYA_PIX_I420, 0, 10) == NULL);
    ASSERT(papaya_frame_get_video(pool, PAPAYA_PIX_I420, 100000, 10) == NULL);
    ASSERT(papaya_frame_get_audio(pool, PAPAYA_SAMPLE_F32, 9, 48000, 10) == NULL);
    papaya_frame_pool_release(&pool);
    PASS();
}

/* ---- queue -------------------------------------------------------------- */

static int queue_freed;

static void
queue_free_item(void *item)
{
    (void)item;
    queue_freed++;
}

TEST
queue_fifo_and_accounting(void)
{
    papaya_queue *q = papaya_queue_new((papaya_queue_limits){ 0 }, NULL);
    void *item;
    int serial;

    ASSERT_EQ(PAPAYA_QUEUE_OK, papaya_queue_push(q, (void *)1, 10, 100));
    ASSERT_EQ(PAPAYA_QUEUE_OK, papaya_queue_push(q, (void *)2, 20, 200));
    ASSERT_EQ(2, papaya_queue_count(q));
    ASSERT_EQ(30u, (unsigned)papaya_queue_bytes(q));
    ASSERT_EQ(300, (int)papaya_queue_duration(q));
    ASSERT_EQ(PAPAYA_QUEUE_OK, papaya_queue_pop(q, &item, &serial, 0));
    ASSERT_EQ((void *)1, item);
    ASSERT_EQ(0, serial);
    ASSERT_EQ(PAPAYA_QUEUE_OK, papaya_queue_pop(q, &item, NULL, 0));
    ASSERT_EQ((void *)2, item);
    ASSERT_EQ(PAPAYA_QUEUE_EMPTY, papaya_queue_pop(q, &item, NULL, 0));
    ASSERT_EQ(PAPAYA_QUEUE_TIMEOUT, papaya_queue_pop(q, &item, NULL, 2000));
    papaya_queue_free(&q);
    PASS();
}

TEST
queue_flush_bumps_serial_and_frees(void)
{
    papaya_queue *q = papaya_queue_new((papaya_queue_limits){ 0 }, queue_free_item);
    void *item;
    int serial;

    queue_freed = 0;
    papaya_queue_push(q, (void *)1, 1, 0);
    papaya_queue_push(q, (void *)2, 1, 0);
    ASSERT_EQ(1, papaya_queue_flush(q));
    ASSERT_EQ(2, queue_freed);
    ASSERT_EQ(0, papaya_queue_count(q));
    papaya_queue_push(q, (void *)3, 1, 0);
    ASSERT_EQ(PAPAYA_QUEUE_OK, papaya_queue_pop(q, &item, &serial, 0));
    ASSERT_EQ(1, serial);
    papaya_queue_push(q, (void *)4, 1, 0);
    papaya_queue_free(&q);
    ASSERT_EQ(3, queue_freed);
    PASS();
}

enum { PC_ITEMS = 10000 };

typedef struct {
    papaya_queue *q;
    int n;
    int sum;
    int values[PC_ITEMS];
} pc_args;

static int
producer(void *p)
{
    pc_args *a = p;

    for (int i = 0; i < a->n; i++) {
        a->values[i] = i + 1;
        if (papaya_queue_push(a->q, &a->values[i], 1, 1000) != PAPAYA_QUEUE_OK)
            return 1;
    }
    return 0;
}

static int
consumer(void *p)
{
    pc_args *a = p;
    void *item;

    for (int i = 0; i < a->n; i++) {
        if (papaya_queue_pop(a->q, &item, NULL, -1) != PAPAYA_QUEUE_OK)
            return 1;
        if (*(int *)item != i + 1)
            return 2;
        a->sum += *(int *)item;
    }
    return 0;
}

TEST
queue_limits_block_producer(void)
{
    /* At most 4 items or 3 ms queued: the producer must wait for the consumer. */
    static pc_args a;
    papaya_thread *pt, *ct;

    a.q = papaya_queue_new((papaya_queue_limits){ .max_duration = 3000, .max_items = 4 }, NULL);
    a.n = PC_ITEMS;
    a.sum = 0;
    pt = papaya_thread_start(producer, &a, "producer");
    ct = papaya_thread_start(consumer, &a, "consumer");
    ASSERT(pt && ct);
    ASSERT_EQ(0, papaya_thread_join(pt));
    ASSERT_EQ(0, papaya_thread_join(ct));
    ASSERT_EQ(PC_ITEMS * (PC_ITEMS + 1) / 2, a.sum);
    papaya_queue_free(&a.q);
    PASS();
}

static int
blocked_pop(void *p)
{
    void *item;

    return papaya_queue_pop((papaya_queue *)p, &item, NULL, -1);
}

TEST
queue_abort_wakes_waiters(void)
{
    papaya_queue *q = papaya_queue_new((papaya_queue_limits){ 0 }, NULL);
    papaya_thread *t = papaya_thread_start(blocked_pop, q, "waiter");

    papaya_sleep_us(20000);
    papaya_queue_abort(q);
    ASSERT_EQ(PAPAYA_QUEUE_ABORTED, papaya_thread_join(t));
    ASSERT_EQ(PAPAYA_QUEUE_ABORTED, papaya_queue_push(q, NULL, 0, 0));
    papaya_queue_resume(q);
    ASSERT_EQ(PAPAYA_QUEUE_OK, papaya_queue_push(q, NULL, 0, 0));
    papaya_queue_free(&q);
    PASS();
}

/* ---- clock -------------------------------------------------------------- */

TEST
clock_advances_pauses_and_scales(void)
{
    papaya_clock c;
    int64_t now = papaya_time_us(), t;
    int serial;

    papaya_clock_init(&c);
    ASSERT(papaya_clock_get(&c, NULL) == PAPAYA_NOPTS);
    papaya_clock_set_at(&c, 1000000, 3, now - 500000);
    t = papaya_clock_get(&c, &serial);
    ASSERT_EQ(3, serial);
    ASSERT(t >= 1500000 && t < 1700000);
    papaya_clock_set_paused(&c, 1);
    t = papaya_clock_get(&c, NULL);
    papaya_sleep_us(30000);
    ASSERT_EQ(t, papaya_clock_get(&c, NULL));
    papaya_clock_set_paused(&c, 0);
    papaya_clock_set_speed(&c, 2.0);
    papaya_clock_set_at(&c, 0, 3, papaya_time_us() - 100000);
    t = papaya_clock_get(&c, NULL);
    ASSERT(t >= 200000 && t < 400000);
    papaya_clock_destroy(&c);
    PASS();
}

SUITE(core)
{
    RUN_TEST(version_matches_header);
    RUN_TEST(buffer_alloc_is_aligned_and_padded);
    RUN_TEST(buffer_wrap_frees_on_last_unref);
    RUN_TEST(packet_copies_data);
    RUN_TEST(pool_video_planes);
    RUN_TEST(pool_reuses_frames);
    RUN_TEST(pool_outlives_release_while_frames_held);
    RUN_TEST(pool_rejects_bad_sizes);
    RUN_TEST(queue_fifo_and_accounting);
    RUN_TEST(queue_flush_bumps_serial_and_frees);
    RUN_TEST(queue_limits_block_producer);
    RUN_TEST(queue_abort_wakes_waiters);
    RUN_TEST(clock_advances_pauses_and_scales);
}

GREATEST_MAIN_DEFS();

int
main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(core);
    GREATEST_MAIN_END();
}
