/* libxaac's encoder, for the AAC round-trip test: raw access units and the
 * AudioSpecificConfig, as an MP4 would carry them. Built without our warnings,
 * since it includes the encoder's headers. */
#include <stdlib.h>
#include <string.h>

#include "aac_encode.h"
#include "ixheaac_type_def.h"
#include "impd_drc_common_enc.h"
#include "impd_drc_uni_drc.h"
#include "impd_drc_tables.h"
#include "impd_drc_api.h"
#include "iusace_cnst.h"
#include "ixheaace_api.h"
#include "ixheaac_error_standards.h"

static pVOID
aligned(UWORD32 size, UWORD32 align)
{
    /* The encoder frees through free_aligned below, with the offset stored. */
    unsigned char *p = malloc(size + align + sizeof(void *));
    unsigned char *q;

    if (!p)
        return NULL;
    q = p + sizeof(void *);
    q += (align - (size_t)q % align) % align;
    memcpy(q - sizeof(void *), (const void *)&p, sizeof p);
    return q;
}

static VOID
free_aligned(pVOID q)
{
    void *p;

    if (!q)
        return;
    memcpy((void *)&p, (unsigned char *)q - sizeof(void *), sizeof p);
    free(p);
}

int
aac_encode(int aot, int adts, int rate, int channels, int bitrate, const short *pcm, int samples,
           void (*au)(void *user, const unsigned char *data, int size), void *user,
           unsigned char *config, int *config_size)
{
    ixheaace_user_config_struct api;
    ixheaace_input_config *in = &api.input_config;
    ixheaace_output_config *out = &api.output_config;
    const unsigned char *src = (const unsigned char *)pcm;
    long left = (long)samples * channels * 2;
    unsigned char *inbuf, *outbuf;
    int frames = 0;

    memset(&api, 0, sizeof api);
    in->aac_config.bitrate = bitrate;
    in->aac_config.inv_quant = 2;
    in->aac_config.use_tns = 1;
    in->aac_config.bitreservoir_size = 768;
    in->i_bitrate = bitrate;
    in->aot = aot;
    in->codec_mode = USAC_ONLY_FD;
    in->i_channels = channels;
    in->i_samp_freq = (UWORD32)rate;
    in->ui_pcm_wd_sz = 16;
    in->i_mps_tree_config = -1;
    in->i_use_adts = adts;
    in->i_use_es = !adts;
    in->ccfl_idx = NO_SBR_CCFL_1024;
    in->frame_length = 1024;
    in->random_access_interval = DEFAULT_RAP_INTERVAL_IN_MS;
    in->method_def = METHOD_DEFINITION_PROGRAM_LOUDNESS;
    in->measurement_system = MEASUREMENT_SYSTEM_BS_1770_3;
    in->pv_drc_cfg = aligned(sizeof(ia_drc_input_config), 8);
    if (!in->pv_drc_cfg)
        return -1;
    memset(in->pv_drc_cfg, 0, sizeof(ia_drc_input_config));
    out->malloc_xheaace = aligned;
    out->free_xheaace = free_aligned;

    if (ixheaace_create(in, out) & IA_FATAL_ERROR) {
        free_aligned(in->pv_drc_cfg);
        return -1;
    }
    inbuf = out->mem_info_table[IA_MEMTYPE_INPUT].mem_ptr;
    outbuf = out->mem_info_table[IA_MEMTYPE_OUTPUT].mem_ptr;
    *config_size = out->i_out_bytes;
    memcpy(config, outbuf, (size_t)out->i_out_bytes);

    /* Each call takes input_size bytes; zeros after the end flush the delay. */
    for (int tail = 0; left > 0 || tail < 4;) {
        long n = left < out->input_size ? left : out->input_size;

        memcpy(inbuf, src, (size_t)(n > 0 ? n : 0));
        memset(inbuf + (n > 0 ? n : 0), 0, (size_t)(out->input_size - (n > 0 ? n : 0)));
        if (ixheaace_process(out->pv_ia_process_api_obj, in, out) & IA_FATAL_ERROR)
            break;
        if (out->i_out_bytes > 0) {
            au(user, outbuf, out->i_out_bytes);
            frames++;
        }
        if (left > 0) {
            src += n;
            left -= n;
        } else
            tail++;
    }
    ixheaace_delete(out);
    free_aligned(in->pv_drc_cfg);
    return frames;
}
