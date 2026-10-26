// WebM / Matroska through libwebm's mkvparser (TECH-STACK.md, CODECS.md 3).
// mkvparser is C++; this file is the only place that sees it, and the rest
// of PapayaPlayer talks to it through papaya_demux_backend.
#include <cstdint>
#include <cstring>
#include <new>

#include "demux_internal.h"
#include "mkvparser/mkvparser.h"

namespace {

class IoReader : public mkvparser::IMkvReader {
public:
    explicit IoReader(papaya_io *io) : io_(io) {}

    int Read(long long pos, long len, unsigned char *buf) override
    {
        if (pos < 0 || len < 0)
            return -1;
        if (len == 0)
            return 0;
        return papaya_io_read_at(io_, pos, buf, static_cast<size_t>(len)) == 0 ? 0 : -1;
    }

    int Length(long long *total, long long *available) override
    {
        long long size = io_->size(io_->opaque);

        if (total)
            *total = size;
        if (available)
            *available = size;
        return size < 0 ? -1 : 0;
    }

private:
    papaya_io *io_;
};

struct Webm {
    explicit Webm(papaya_io *io) : reader(io) {}
    ~Webm() { delete segment; }

    IoReader reader;
    mkvparser::Segment *segment = nullptr;
    // Matroska track number -> our stream index, -1 for tracks we skip.
    int track_to_stream[256];
    const mkvparser::Cluster *cluster = nullptr;
    const mkvparser::BlockEntry *entry = nullptr;
    int frame = 0;
    bool started = false;
    const mkvparser::Track *seek_track = nullptr;
};

int64_t
ns_to_us(long long ns)
{
    return ns < 0 ? PAPAYA_NOPTS : static_cast<int64_t>(ns / 1000);
}

papaya_codec
codec_from_id(const char *id)
{
    static const struct {
        const char *id;
        papaya_codec codec;
    } map[] = {
        { "V_AV1", PAPAYA_CODEC_AV1 },
        { "V_VP8", PAPAYA_CODEC_VP8 },
        { "V_VP9", PAPAYA_CODEC_VP9 },
        { "V_MPEG4/ISO/AVC", PAPAYA_CODEC_H264 },
        { "V_MPEGH/ISO/HEVC", PAPAYA_CODEC_HEVC },
        { "V_MPEG2", PAPAYA_CODEC_MPEG2 },
        { "A_OPUS", PAPAYA_CODEC_OPUS },
        { "A_VORBIS", PAPAYA_CODEC_VORBIS },
        { "A_FLAC", PAPAYA_CODEC_FLAC },
        { "A_AAC", PAPAYA_CODEC_AAC },
        { "A_MPEG/L3", PAPAYA_CODEC_MP3 },
        { "A_AC3", PAPAYA_CODEC_AC3 },
        { "A_EAC3", PAPAYA_CODEC_EAC3 },
        { "A_PCM/INT/LIT", PAPAYA_CODEC_PCM_S16LE },
        { "A_PCM/FLOAT/IEEE", PAPAYA_CODEC_PCM_F32LE },
    };

    if (!id)
        return PAPAYA_CODEC_UNKNOWN;
    for (const auto &m : map)
        if (std::strcmp(id, m.id) == 0)
            return m.codec;
    if (std::strncmp(id, "A_AAC/", 6) == 0)
        return PAPAYA_CODEC_AAC;
    return PAPAYA_CODEC_UNKNOWN;
}

uint8_t
code_point(long long v)
{
    return v >= 0 && v <= 255 ? static_cast<uint8_t>(v) : 2; // 2: unspecified
}

void
fill_color(papaya_color *c, const mkvparser::Colour *colour)
{
    c->primaries = c->transfer = c->matrix = 2;
    if (!colour)
        return;
    c->primaries = code_point(colour->primaries);
    c->transfer = code_point(colour->transfer_characteristics);
    c->matrix = code_point(colour->matrix_coefficients);
    c->full_range = colour->range == 2;
    if (colour->max_cll > 0)
        c->max_cll = static_cast<uint16_t>(colour->max_cll);
    if (colour->max_fall > 0)
        c->max_fall = static_cast<uint16_t>(colour->max_fall);
    if (const mkvparser::MasteringMetadata *m = colour->mastering_metadata) {
        if (m->luminance_max > 0)
            c->max_luminance = m->luminance_max;
        if (m->luminance_min > 0)
            c->min_luminance = m->luminance_min;
    }
}

int
webm_probe(const uint8_t *head, size_t size)
{
    // EBML magic; both the "webm" and "matroska" doctypes are read.
    return size >= 4 && head[0] == 0x1A && head[1] == 0x45 && head[2] == 0xDF && head[3] == 0xA3
        ? 100 : 0;
}

void *
webm_open(papaya_demuxer *d, papaya_io *io)
{
    Webm *w = new (std::nothrow) Webm(io);
    mkvparser::EBMLHeader ebml;
    long long pos = 0;

    if (!w)
        return nullptr;
    for (int &s : w->track_to_stream)
        s = -1;
    // Load parses the headers and indexes every cluster, which seeking without
    // Cues needs. Fine for local files; streaming will load incrementally.
    if (ebml.Parse(&w->reader, pos) < 0
        || mkvparser::Segment::CreateInstance(&w->reader, pos, w->segment) != 0
        || !w->segment || w->segment->Load() < 0) {
        delete w;
        return nullptr;
    }

    const mkvparser::Tracks *tracks = w->segment->GetTracks();
    const mkvparser::SegmentInfo *info = w->segment->GetInfo();
    if (!tracks) {
        delete w;
        return nullptr;
    }
    if (info && info->GetDuration() > 0)
        papaya_demux_set_duration(d, ns_to_us(info->GetDuration()));

    for (unsigned long i = 0; i < tracks->GetTracksCount(); i++) {
        const mkvparser::Track *t = tracks->GetTrackByIndex(i);
        papaya_stream_info *s;
        long type, number;
        size_t private_size = 0;

        if (!t)
            continue;
        type = t->GetType();
        number = t->GetNumber();
        if ((type != mkvparser::Track::kVideo && type != mkvparser::Track::kAudio)
            || number <= 0 || number >= 256)
            continue;
        s = papaya_demux_add_stream(d);
        if (!s)
            break;
        w->track_to_stream[number] = s->index;
        s->codec_tag = t->GetCodecId();
        s->codec = codec_from_id(s->codec_tag);
        s->extradata = t->GetCodecPrivate(private_size);
        s->extradata_size = private_size;
        if (const char *lang = t->GetLanguage())
            std::strncpy(s->language, lang, sizeof s->language - 1);
        if (const char *name = t->GetNameAsUTF8())
            std::strncpy(s->title, name, sizeof s->title - 1);
        s->codec_delay = ns_to_us(static_cast<long long>(t->GetCodecDelay()));
        s->seek_preroll = ns_to_us(static_cast<long long>(t->GetSeekPreRoll()));
        if (type == mkvparser::Track::kVideo) {
            auto *v = static_cast<const mkvparser::VideoTrack *>(t);

            s->kind = PAPAYA_MEDIA_VIDEO;
            s->width = static_cast<int>(v->GetWidth());
            s->height = static_cast<int>(v->GetHeight());
            fill_color(&s->color, v->GetColour());
            if (!w->seek_track)
                w->seek_track = t;
        } else {
            auto *a = static_cast<const mkvparser::AudioTrack *>(t);

            s->kind = PAPAYA_MEDIA_AUDIO;
            s->sample_rate = static_cast<int>(a->GetSamplingRate());
            s->channels = static_cast<int>(a->GetChannels());
            s->bits_per_sample = static_cast<int>(a->GetBitDepth());
        }
    }
    if (!w->seek_track) {
        for (unsigned long i = 0; i < tracks->GetTracksCount(); i++) {
            const mkvparser::Track *t = tracks->GetTrackByIndex(i);

            if (t && t->GetNumber() > 0 && t->GetNumber() < 256
                && w->track_to_stream[t->GetNumber()] >= 0) {
                w->seek_track = t;
                break;
            }
        }
    }
    return w;
}

// Moves to the next block, crossing clusters: 0 found, 1 end, < 0 error.
long
next_entry(Webm *w)
{
    const mkvparser::BlockEntry *next = nullptr;

    if (w->entry && w->cluster->GetNext(w->entry, next) < 0)
        return -1;
    while (!next || next->EOS()) {
        w->cluster = w->segment->GetNext(w->cluster);
        if (!w->cluster || w->cluster->EOS()) {
            w->entry = nullptr;
            return 1;
        }
        if (w->cluster->GetFirst(next) < 0)
            return -1;
    }
    w->entry = next;
    w->frame = 0;
    return 0;
}

int
webm_read(void *ctx, papaya_packet **packet)
{
    Webm *w = static_cast<Webm *>(ctx);

    if (!w->started) {
        w->started = true;
        w->cluster = w->segment->GetFirst();
        if (!w->cluster || w->cluster->EOS())
            return PAPAYA_DEMUX_EOF;
        w->entry = nullptr;
        if (w->cluster->GetFirst(w->entry) < 0)
            return PAPAYA_DEMUX_ERROR;
        w->frame = 0;
        if (!w->entry || w->entry->EOS()) {
            w->entry = nullptr;
            long r = next_entry(w);
            if (r)
                return r > 0 ? PAPAYA_DEMUX_EOF : PAPAYA_DEMUX_ERROR;
        }
    }
    while (w->entry) {
        const mkvparser::Block *b = w->entry->GetBlock();
        long long track = b ? b->GetTrackNumber() : -1;
        int stream = track > 0 && track < 256 ? w->track_to_stream[track] : -1;

        if (!b || stream < 0 || w->frame >= b->GetFrameCount()) {
            long r = next_entry(w);
            if (r)
                return r > 0 ? PAPAYA_DEMUX_EOF : PAPAYA_DEMUX_ERROR;
            continue;
        }
        const mkvparser::Block::Frame &f = b->GetFrame(w->frame);
        if (f.len <= 0)
            return PAPAYA_DEMUX_ERROR;
        papaya_packet *p = papaya_packet_new(nullptr, static_cast<size_t>(f.len));
        if (!p)
            return PAPAYA_DEMUX_ERROR;
        if (f.Read(&w->reader, papaya_buffer_data(p->buf)) < 0) {
            papaya_packet_free(&p);
            return PAPAYA_DEMUX_ERROR;
        }
        // Laced frames after the first carry no time of their own.
        p->pts = w->frame == 0 ? ns_to_us(b->GetTime(w->cluster)) : PAPAYA_NOPTS;
        p->dts = PAPAYA_NOPTS;
        p->stream = stream;
        if (b->IsKey())
            p->flags |= PAPAYA_PACKET_KEY;
        w->frame++;
        *packet = p;
        return 0;
    }
    return PAPAYA_DEMUX_EOF;
}

int
webm_seek(void *ctx, int64_t pts)
{
    Webm *w = static_cast<Webm *>(ctx);
    const mkvparser::BlockEntry *e = nullptr;
    long long ns = pts <= 0 ? 0 : static_cast<long long>(pts) * 1000;

    if (!w->seek_track)
        return PAPAYA_DEMUX_ERROR;
    if (w->seek_track->Seek(ns, e) < 0 || !e)
        return PAPAYA_DEMUX_ERROR;
    w->started = true;
    if (e->EOS()) {
        w->entry = nullptr;
        return 0;
    }
    w->cluster = e->GetCluster();
    w->entry = e;
    w->frame = 0;
    return 0;
}

void
webm_close(void *ctx)
{
    delete static_cast<Webm *>(ctx);
}

} // namespace

extern "C" const papaya_demux_backend papaya_demux_webm = {
    "webm", webm_probe, webm_open, webm_read, webm_seek, webm_close,
};
