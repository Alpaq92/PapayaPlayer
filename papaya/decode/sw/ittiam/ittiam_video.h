/* The shared body of the libavc (H.264) and libhevc (HEVC) backends: both
 * Ittiam decoders speak the same ivd_* API and differ only in names. The
 * including file defines:
 *   ITT_PREFIX(x)   the library's prefix for its own structs (ih264d_x)
 *   ITT_API         its entry point
 *   ITT_CORES_CMD   its "set number of cores" control
 *   ITT_CREATE_EXTRA(ip)  extra create fields, may be empty
 *   ITT_BIT_DEPTH(op)     a decode op's luma bit depth (8 where the API has none)
 *   ITT_STREAM_COLOR(c)   optional: fills c->color from the bitstream once its
 *                         headers are in (what the container left unspecified)
 *   ITT_PICTURE_COLOR(c, f) optional: per-picture colour metadata (SEI) into f
 *   ITT_CHROMA_FORMAT(p, n) optional: the chroma_format_idc of the first SPS
 *                           in Annex B data, or -1; the decoder is created again
 *                           for 4:2:2 / 4:4:4 output
 * and the parameter-set parser itt_to_annexb_config for its configuration
 * record (avcC / hvcC).
 *
 * Packets from MP4 and Matroska are length-prefixed; the decoders want
 * Annex B start codes, so each packet is rewritten. Output is in display
 * order, so packet times travel through the decoder's u4_ts tag. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ITT_TS_SLOTS 64

typedef struct {
    iv_obj_t *codec;
    papaya_frame_pool *pool;
    int length_size; /* 0: packets are already Annex B */
    int header_done;
    int width, height;
    int sample_bytes;  /* 2 for 16-bit (high bit depth) output */
    int chroma;        /* chroma_format_idc of the output: 1 4:2:0, 2 4:2:2, 3 4:4:4 */
    papaya_color color;
    uint8_t *out[3];   /* our copy of the decoded picture (share_disp_buf 0) */
    size_t out_size[3];
    uint8_t *annexb;   /* the rewritten packet */
    size_t annexb_cap;
    uint8_t *config;   /* parameter sets as Annex B */
    size_t config_size;
    uint32_t next_ts;
    int64_t pts[ITT_TS_SLOTS], duration[ITT_TS_SLOTS];
} itt_ctx;

static void itt_close(void *ctx);
static int itt_create(itt_ctx *c);
static void itt_delete(itt_ctx *c);
static int itt_config_nal(itt_ctx *c, const uint8_t *p, size_t n);
static int itt_to_annexb_config(itt_ctx *c, const uint8_t *p, size_t n);

/* Aligned blocks, the same on every OS: malloc with room to align, and the
 * block's own pointer kept just before the aligned one. */
static void *
itt_alloc(void *ctx, WORD32 alignment, WORD32 size)
{
    size_t align = alignment > (WORD32)sizeof(void *) ? (size_t)alignment : sizeof(void *);
    unsigned char *raw, *p;

    (void)ctx;
    if (size <= 0 || (align & (align - 1)))
        return NULL;
    raw = malloc((size_t)size + align + sizeof(void *));
    if (!raw)
        return NULL;
    p = raw + sizeof(void *);
    p += (align - (uintptr_t)p % align) % align;
    memcpy(p - sizeof(void *), (const void *)&raw, sizeof raw);
    return p;
}

static void
itt_free(void *ctx, void *p)
{
    void *raw;

    (void)ctx;
    if (!p)
        return;
    memcpy((void *)&raw, (unsigned char *)p - sizeof(void *), sizeof raw);
    free(raw);
}

static int
itt_set_mode(itt_ctx *c, IVD_VIDEO_DECODE_MODE_T mode)
{
    ITT_PREFIX(ctl_set_config_ip_t) ip;
    ITT_PREFIX(ctl_set_config_op_t) op;

    memset(&ip, 0, sizeof ip);
    memset(&op, 0, sizeof op);
    ip.s_ivd_ctl_set_config_ip_t.u4_disp_wd = (UWORD32)c->width;
    ip.s_ivd_ctl_set_config_ip_t.e_frm_skip_mode = IVD_SKIP_NONE;
    ip.s_ivd_ctl_set_config_ip_t.e_frm_out_mode = IVD_DISPLAY_FRAME_OUT;
    ip.s_ivd_ctl_set_config_ip_t.e_vid_dec_mode = mode;
    ip.s_ivd_ctl_set_config_ip_t.e_cmd = IVD_CMD_VIDEO_CTL;
    ip.s_ivd_ctl_set_config_ip_t.e_sub_cmd = IVD_CMD_CTL_SETPARAMS;
    ip.s_ivd_ctl_set_config_ip_t.u4_size = sizeof ip;
    op.s_ivd_ctl_set_config_op_t.u4_size = sizeof op;
    return ITT_API(c->codec, &ip, &op) == IV_SUCCESS ? 0 : -1;
}

static int
itt_setup(itt_ctx *c)
{
    ITT_PREFIX(ctl_set_num_cores_ip_t) cores;
    ITT_PREFIX(ctl_set_num_cores_op_t) cores_op;

    memset(&cores, 0, sizeof cores);
    memset(&cores_op, 0, sizeof cores_op);
    cores.e_cmd = IVD_CMD_VIDEO_CTL;
    cores.e_sub_cmd = (IVD_CONTROL_API_COMMAND_TYPE_T)ITT_CORES_CMD;
    cores.u4_num_cores = 4;
    cores.u4_size = sizeof cores;
    cores_op.u4_size = sizeof cores_op;
    if (ITT_API(c->codec, &cores, &cores_op) != IV_SUCCESS)
        return -1;
    /* The processor stays at the build's DEFAULT_ARCH (SSE4.2 on x86-64):
     * libavc's AVX2 path fails the JVT conformance streams. */
    return itt_set_mode(c, IVD_DECODE_HEADER);
}

/* One decode call; *consumed is how many bytes it took. */
static IV_API_CALL_STATUS_T
itt_call(itt_ctx *c, const uint8_t *data, size_t size, uint32_t ts, ITT_PREFIX(video_decode_op_t) *op)
{
    ITT_PREFIX(video_decode_ip_t) ip;
    ivd_video_decode_ip_t *in = &ip.s_ivd_video_decode_ip_t;

    memset(&ip, 0, sizeof ip);
    memset(op, 0, sizeof *op);
    in->e_cmd = IVD_CMD_VIDEO_DECODE;
    in->u4_ts = ts;
    in->pv_stream_buffer = (void *)data;
    in->u4_num_Bytes = (UWORD32)size;
    in->u4_size = sizeof ip;
    in->s_out_buffer.u4_num_bufs = 3;
    for (int i = 0; i < 3; i++) {
        in->s_out_buffer.pu1_bufs[i] = c->out[i];
        in->s_out_buffer.u4_min_out_buf_size[i] = (UWORD32)c->out_size[i];
    }
    op->s_ivd_video_decode_op_t.u4_size = sizeof *op;
    return ITT_API(c->codec, &ip, op);
}

/* Parses parameter sets until the decoder knows the picture size, then
 * switches it to frame decoding with output buffers for that size. */
static int
itt_headers(itt_ctx *c, const uint8_t *data, size_t size)
{
    ITT_PREFIX(video_decode_op_t) op;

#ifdef ITT_CHROMA_FORMAT
    {
        int cf = ITT_CHROMA_FORMAT(data, size);

        if (cf >= 1 && cf <= 3 && cf != c->chroma) {
            itt_delete(c);
            c->chroma = cf;
            if (itt_create(c) != 0)
                return -1;
        }
    }
#endif
    while (size > 0 && !c->header_done) {
        uint32_t used;

        itt_call(c, data, size, 0, &op);
        used = op.s_ivd_video_decode_op_t.u4_num_bytes_consumed;
        if (op.s_ivd_video_decode_op_t.u4_pic_wd && op.s_ivd_video_decode_op_t.u4_pic_ht) {
            c->width = (int)op.s_ivd_video_decode_op_t.u4_pic_wd;
            c->height = (int)op.s_ivd_video_decode_op_t.u4_pic_ht;
            c->sample_bytes = ITT_BIT_DEPTH(&op.s_ivd_video_decode_op_t) > 8 ? 2 : 1;
            c->header_done = 1;
#ifdef ITT_STREAM_COLOR
            ITT_STREAM_COLOR(c);
#endif
        }
        if (used == 0 || used > size)
            break;
        data += used;
        size -= used;
    }
    if (!c->header_done)
        return 0;
    {
        size_t luma = (size_t)c->width * (size_t)c->height * (size_t)c->sample_bytes;

        c->out_size[0] = luma;
        c->out_size[1] = c->out_size[2] = (c->chroma == 3 ? luma : c->chroma == 2 ? luma / 2 : luma / 4)
                                          + (size_t)c->width * (size_t)c->sample_bytes;
        for (int i = 0; i < 3; i++) {
            free(c->out[i]);
            c->out[i] = malloc(c->out_size[i]);
            if (!c->out[i])
                return -1;
        }
    }
    return itt_set_mode(c, IVD_DECODE_FRAME);
}

static uint8_t *
itt_reserve(itt_ctx *c, size_t n)
{
    if (n > c->annexb_cap) {
        uint8_t *grown = realloc(c->annexb, n);

        if (!grown)
            return NULL;
        c->annexb = grown;
        c->annexb_cap = n;
    }
    return c->annexb;
}

/* Length-prefixed NAL units to start codes; returns the new size or 0. */
static size_t
itt_rewrite(itt_ctx *c, const uint8_t *p, size_t n)
{
    size_t out = 0;

    if (c->length_size == 0) {
        if (!itt_reserve(c, n))
            return 0;
        memcpy(c->annexb, p, n);
        return n;
    }
    while (n >= (size_t)c->length_size) {
        size_t len = 0;

        for (int i = 0; i < c->length_size; i++)
            len = len << 8 | p[i];
        p += c->length_size;
        n -= (size_t)c->length_size;
        if (len > n)
            len = n;
        if (!itt_reserve(c, out + 4 + len))
            return 0;
        memcpy(c->annexb + out, "\0\0\0\1", 4);
        memcpy(c->annexb + out + 4, p, len);
        out += 4 + len;
        p += len;
        n -= len;
    }
    return out;
}

static papaya_frame *
itt_picture(itt_ctx *c, const ivd_video_decode_op_t *op)
{
    const iv_yuv_buf_t *b = &op->s_disp_frm_buf;
    int w = (int)b->u4_y_wd, h = (int)b->u4_y_ht;
    size_t bytes = ITT_BIT_DEPTH(op) > 8 ? 2 : 1; /* 16-bit samples above 8 bits */
    papaya_pixel_format pix = c->chroma == 3   ? (bytes == 2 ? PAPAYA_PIX_I444_10 : PAPAYA_PIX_I444)
                              : c->chroma == 2 ? (bytes == 2 ? PAPAYA_PIX_I422_10 : PAPAYA_PIX_I422)
                                               : (bytes == 2 ? PAPAYA_PIX_I420_10 : PAPAYA_PIX_I420);
    papaya_frame *f = papaya_frame_get_video(c->pool, pix, w, h);
    const uint8_t *src[3] = { b->pv_y_buf, b->pv_u_buf, b->pv_v_buf };
    size_t stride[3] = { b->u4_y_strd * bytes, b->u4_u_strd * bytes, b->u4_v_strd * bytes };

    if (!f)
        return NULL;
    for (int p = 0; p < 3; p++) {
        int pw = p && c->chroma != 3 ? (w + 1) / 2 : w, ph = p && c->chroma == 1 ? (h + 1) / 2 : h;

        for (int y = 0; y < ph; y++)
            memcpy(f->planes[p] + (size_t)y * (size_t)f->strides[p], src[p] + (size_t)y * stride[p],
                   (size_t)pw * bytes);
    }
    f->color = c->color;
#ifdef ITT_PICTURE_COLOR
    ITT_PICTURE_COLOR(c, f);
#endif
    if (f->color.matrix == 2) /* unspecified: the usual guess by size */
        f->color.matrix = h > 576 ? 1 : 6;
    f->pts = c->pts[op->u4_ts % ITT_TS_SLOTS];
    f->duration = c->duration[op->u4_ts % ITT_TS_SLOTS];
    return f;
}

/* A decoder instance for c->chroma, set up to parse headers. */
static int
itt_create(itt_ctx *c)
{
    ITT_PREFIX(create_ip_t) ip;
    ITT_PREFIX(create_op_t) op;

    memset(&ip, 0, sizeof ip);
    memset(&op, 0, sizeof op);
    ip.s_ivd_create_ip_t.e_cmd = IVD_CMD_CREATE;
    ip.s_ivd_create_ip_t.u4_share_disp_buf = 0;
    ip.s_ivd_create_ip_t.e_output_format = c->chroma == 3   ? IV_YUV_444P
                                           : c->chroma == 2 ? IV_YUV_422P
                                                            : IV_YUV_420P;
    ip.s_ivd_create_ip_t.pf_aligned_alloc = itt_alloc;
    ip.s_ivd_create_ip_t.pf_aligned_free = itt_free;
    ip.s_ivd_create_ip_t.u4_size = sizeof ip;
    op.s_ivd_create_op_t.u4_size = sizeof op;
    ITT_CREATE_EXTRA(ip);
    if (ITT_API(NULL, &ip, &op) != IV_SUCCESS)
        return -1;
    c->codec = op.s_ivd_create_op_t.pv_handle;
    c->codec->pv_fxns = (void *)ITT_API;
    c->codec->u4_size = sizeof(iv_obj_t);
    return itt_setup(c);
}

static void
itt_delete(itt_ctx *c)
{
    ivd_delete_ip_t ip;
    ivd_delete_op_t op;

    if (!c->codec)
        return;
    memset(&ip, 0, sizeof ip);
    memset(&op, 0, sizeof op);
    ip.e_cmd = IVD_CMD_DELETE;
    ip.u4_size = sizeof ip;
    op.u4_size = sizeof op;
    ITT_API(c->codec, &ip, &op);
    c->codec = NULL;
}

static void *
itt_open(const papaya_stream_info *s, papaya_frame_pool *pool)
{
    itt_ctx *c = calloc(1, sizeof *c);

    if (!c)
        return NULL;
    c->pool = pool;
    c->color = s->color;
    c->width = s->width;
    c->chroma = 1;
    if (itt_create(c) != 0) {
        itt_close(c);
        return NULL;
    }
    if (itt_to_annexb_config(c, s->extradata, s->extradata_size) != 0
        || (c->config_size && itt_headers(c, c->config, c->config_size) != 0)) {
        itt_close(c);
        return NULL;
    }
    return c;
}

static void
itt_drain(itt_ctx *c, void (*emit)(void *, papaya_frame *), void *sink)
{
    ivd_ctl_flush_ip_t ip;
    ivd_ctl_flush_op_t fop;
    ITT_PREFIX(video_decode_op_t) op;

    if (!c->header_done || !c->codec)
        return;
    memset(&ip, 0, sizeof ip);
    memset(&fop, 0, sizeof fop);
    ip.e_cmd = IVD_CMD_VIDEO_CTL;
    ip.e_sub_cmd = IVD_CMD_CTL_FLUSH;
    ip.u4_size = sizeof ip;
    fop.u4_size = sizeof fop;
    if (ITT_API(c->codec, &ip, &fop) != IV_SUCCESS)
        return;
    for (;;) { /* every buffered picture, then none */
        papaya_frame *f;

        itt_call(c, NULL, 0, 0, &op);
        if (!op.s_ivd_video_decode_op_t.u4_output_present)
            break;
        f = emit ? itt_picture(c, &op.s_ivd_video_decode_op_t) : NULL;
        if (f)
            emit(sink, f);
    }
}

static int
itt_decode(void *ctx, const papaya_packet *p, void (*emit)(void *, papaya_frame *), void *sink)
{
    itt_ctx *c = ctx;
    const uint8_t *data;
    size_t size;
    uint32_t ts;

    if (!c->codec)
        return -1; /* lost when created again for another chroma format */
    if (!p) {
        itt_drain(c, emit, sink);
        return 0;
    }
    size = itt_rewrite(c, p->data, p->size);
    if (size == 0)
        return p->size ? -1 : 0;
    data = c->annexb;
    if (!c->header_done) {
        if (itt_headers(c, data, size) != 0)
            return -1;
        if (!c->header_done)
            return 0;
    }
    ts = c->next_ts++;
    c->pts[ts % ITT_TS_SLOTS] = p->pts;
    c->duration[ts % ITT_TS_SLOTS] = p->duration;
    while (size > 0) {
        ITT_PREFIX(video_decode_op_t) op;
        const ivd_video_decode_op_t *o = &op.s_ivd_video_decode_op_t;
        uint32_t used;

        itt_call(c, data, size, ts, &op);
        if (o->u4_output_present) {
            papaya_frame *f = itt_picture(c, o);

            if (!f)
                return -1;
            emit(sink, f);
        }
        if ((o->u4_error_code >> IVD_FATALERROR) & 1)
            return -1;
        used = o->u4_num_bytes_consumed;
        if (used == 0 || used > size)
            break;
        data += used;
        size -= used;
    }
    return 0;
}

static void
itt_flush(void *ctx)
{
    itt_drain(ctx, NULL, NULL);
}

static void
itt_close(void *ctx)
{
    itt_ctx *c = ctx;

    itt_delete(c);
    for (int i = 0; i < 3; i++)
        free(c->out[i]);
    free(c->annexb);
    free(c->config);
    free(c);
}

/* Appends one parameter set to c->config with a start code. */
static int
itt_config_nal(itt_ctx *c, const uint8_t *p, size_t n)
{
    uint8_t *grown = realloc(c->config, c->config_size + 4 + n);

    if (!grown)
        return -1;
    c->config = grown;
    memcpy(c->config + c->config_size, "\0\0\0\1", 4);
    memcpy(c->config + c->config_size + 4, p, n);
    c->config_size += 4 + n;
    return 0;
}
