#include "papaya/audio_out.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniaudio.h"
#include "papaya/queue.h"

typedef struct {
    papaya_frame *frame;
    int serial;
} queued;

struct papaya_audio_out {
    ma_context context;
    ma_device device;
    papaya_queue *frames;
    papaya_clock *clock;
    int channels, rate;
    int started;           /* device is initialised */
    _Atomic int64_t latency;
    char name[256];

    /* Touched only by the device callback. */
    papaya_frame *current;
    int current_serial;
    int offset;            /* samples of current already played */

    atomic_int serial;     /* frames with another serial are stale */
    atomic_int paused;
    _Atomic float volume;
};

static void
free_queued(void *item)
{
    queued *q = item;

    papaya_frame_unref(&q->frame);
    free(q);
}

/* Next frame of the current serial, or NULL. Never blocks. */
static int
take_frame(papaya_audio_out *ao)
{
    void *item;
    int serial = atomic_load(&ao->serial);

    while (papaya_queue_pop(ao->frames, &item, NULL, 0) == PAPAYA_QUEUE_OK) {
        queued *q = item;

        if (q->serial != serial) {
            free_queued(q);
            continue;
        }
        ao->current = q->frame;
        ao->current_serial = q->serial;
        ao->offset = 0;
        free(q);
        return 1;
    }
    return 0;
}

static void
on_data(ma_device *device, void *output, const void *input, ma_uint32 count)
{
    papaya_audio_out *ao = device->pUserData;
    float *out = output;
    float volume = atomic_load(&ao->volume);
    int64_t now = papaya_time_us();
    int64_t first_pts = PAPAYA_NOPTS;
    int clock_serial = 0;
    ma_uint32 done = 0;

    (void)input;
    if (atomic_load(&ao->paused)) {
        memset(output, 0, (size_t)count * (size_t)ao->channels * sizeof(float));
        return;
    }
    if (ao->current && ao->current_serial != atomic_load(&ao->serial))
        papaya_frame_unref(&ao->current);
    while (done < count) {
        int n;

        if (!ao->current && !take_frame(ao))
            break;
        if (first_pts == PAPAYA_NOPTS && ao->current->pts != PAPAYA_NOPTS) {
            first_pts = ao->current->pts + (int64_t)ao->offset * 1000000 / ao->rate
                        - (int64_t)done * 1000000 / ao->rate;
            clock_serial = ao->current_serial;
        }
        n = ao->current->samples - ao->offset;
        if ((ma_uint32)n > count - done)
            n = (int)(count - done);
        {
            const float *src = (const float *)ao->current->audio[0] + (size_t)ao->offset * (size_t)ao->channels;
            float *dst = out + (size_t)done * (size_t)ao->channels;
            size_t total = (size_t)n * (size_t)ao->channels;

            for (size_t i = 0; i < total; i++)
                dst[i] = src[i] * volume;
        }
        ao->offset += n;
        done += (ma_uint32)n;
        if (ao->offset >= ao->current->samples)
            papaya_frame_unref(&ao->current);
    }
    if (done < count)
        memset(out + (size_t)done * (size_t)ao->channels, 0,
               (size_t)(count - done) * (size_t)ao->channels * sizeof(float));
    /* The first sample written now is heard after the device's own buffer. */
    if (ao->clock && first_pts != PAPAYA_NOPTS)
        papaya_clock_set_at(ao->clock, first_pts - ao->latency, clock_serial, now);
}

/* The device for ao's rate and channels, started. */
static int
start_device(papaya_audio_out *ao)
{
    ma_device_config dc = ma_device_config_init(ma_device_type_playback);

    dc.playback.format = ma_format_f32;
    dc.playback.channels = (ma_uint32)ao->channels;
    dc.sampleRate = (ma_uint32)ao->rate;
    dc.dataCallback = on_data;
    dc.pUserData = ao;
    dc.performanceProfile = ma_performance_profile_low_latency;
    /* Our frames are WAVE order; miniaudio's default map for these counts
     * is the same, and it remaps if the device differs. */
    if (ma_device_init(&ao->context, &dc, &ao->device) != MA_SUCCESS)
        return -1;
    atomic_store(&ao->latency, (int64_t)ao->device.playback.internalPeriodSizeInFrames
                                   * ao->device.playback.internalPeriods * 1000000
                                   / (ao->device.playback.internalSampleRate ? ao->device.playback.internalSampleRate
                                                                             : 48000));
    if (ma_device_start(&ao->device) != MA_SUCCESS) {
        ma_device_uninit(&ao->device);
        return -1;
    }
    ao->started = 1;
    return 0;
}

papaya_audio_out *
papaya_audio_out_open(const papaya_audio_out_config *config)
{
    papaya_audio_out *ao;
    ma_backend null_backend = ma_backend_null;
    ma_device_info *playback;
    ma_uint32 playback_count = 0;
    int64_t buffer = config->buffer_duration > 0 ? config->buffer_duration : 300000;

    if (config->sample_rate <= 0 || config->channels < 1 || config->channels > 8)
        return NULL;
    ao = calloc(1, sizeof *ao);
    if (!ao)
        return NULL;
    ao->channels = config->channels;
    ao->rate = config->sample_rate;
    ao->clock = config->clock;
    atomic_init(&ao->volume, 1.0f);
    ao->frames = papaya_queue_new((papaya_queue_limits){ .max_duration = buffer }, free_queued);
    if (!ao->frames)
        goto fail;
    if (ma_context_init(config->null_device ? &null_backend : NULL, config->null_device ? 1 : 0,
                        NULL, &ao->context) != MA_SUCCESS)
        goto fail_queue;

    if (start_device(ao) != 0)
        goto fail_context;
    if (ma_context_get_devices(&ao->context, &playback, &playback_count, NULL, NULL) == MA_SUCCESS
        && playback_count)
        ma_device_get_name(&ao->device, ma_device_type_playback, ao->name, sizeof ao->name, NULL);
    if (!ao->name[0])
        snprintf(ao->name, sizeof ao->name, "%s", config->null_device ? "null" : "default");
    return ao;
fail_context:
    ma_context_uninit(&ao->context);
fail_queue:
    papaya_queue_free(&ao->frames);
fail:
    free(ao);
    return NULL;
}

void
papaya_audio_out_close(papaya_audio_out **pao)
{
    papaya_audio_out *ao = *pao;

    *pao = NULL;
    if (!ao)
        return;
    papaya_queue_abort(ao->frames);
    if (ao->started)
        ma_device_uninit(&ao->device);
    ma_context_uninit(&ao->context);
    papaya_frame_unref(&ao->current);
    papaya_queue_free(&ao->frames);
    free(ao);
}

const char *
papaya_audio_out_device_name(const papaya_audio_out *ao)
{
    return ao->name;
}

int
papaya_audio_out_write(papaya_audio_out *ao, papaya_frame *frame, int serial)
{
    queued *q;

    if (frame->kind != PAPAYA_MEDIA_AUDIO || frame->sample != PAPAYA_SAMPLE_F32 || frame->channels < 1
        || frame->channels > 8 || frame->sample_rate <= 0)
        return -1;
    if (frame->channels != ao->channels || frame->sample_rate != ao->rate) {
        /* The decoder knows better than the container (HE-AAC's SBR and PS
         * double the rate or the channels): a device for the frames. What is
         * queued in the old format goes; at the start there is nothing. */
        if (ao->started)
            ma_device_uninit(&ao->device);
        ao->started = 0;
        papaya_frame_unref(&ao->current); /* the callback has stopped */
        papaya_queue_flush(ao->frames);
        ao->channels = frame->channels;
        ao->rate = frame->sample_rate;
        if (start_device(ao) != 0)
            return -1;
    }
    q = malloc(sizeof *q);
    if (!q)
        return -1;
    q->frame = papaya_frame_ref(frame);
    q->serial = serial;
    if (papaya_queue_push(ao->frames, q, 0, (int64_t)frame->samples * 1000000 / ao->rate)
        != PAPAYA_QUEUE_OK) {
        free_queued(q);
        return -1;
    }
    return 0;
}

void
papaya_audio_out_flush(papaya_audio_out *ao, int serial)
{
    atomic_store(&ao->serial, serial);
    papaya_queue_flush(ao->frames);
}

void
papaya_audio_out_abort(papaya_audio_out *ao)
{
    papaya_queue_abort(ao->frames);
}

void
papaya_audio_out_set_paused(papaya_audio_out *ao, int paused)
{
    atomic_store(&ao->paused, paused);
    if (ao->clock)
        papaya_clock_set_paused(ao->clock, paused);
}

void
papaya_audio_out_set_volume(papaya_audio_out *ao, float volume)
{
    atomic_store(&ao->volume, volume < 0 ? 0 : volume);
}

int64_t
papaya_audio_out_queued(papaya_audio_out *ao)
{
    return papaya_queue_duration(ao->frames);
}

int64_t
papaya_audio_out_latency(const papaya_audio_out *ao)
{
    return atomic_load(&ao->latency);
}
