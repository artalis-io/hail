#include "consumer.h"
#include <string.h>

static ha_result track(dsp_timeline *t, const ha_block_info *b)
{
    int changed = !t->seen || t->domain != b->clock_domain || t->epoch != b->clock_epoch;
    if (t->seen && !changed && b->first_frame < t->end) return HA_STREAM_FAILED;
    if (t->time_seen && !changed && (b->flags & HA_TIME_HOST_VALID) && b->host_time_ns < t->host)
        return HA_STREAM_FAILED;
    if (changed) {
        ++t->resets; t->time_seen = 0;
    }
    if (b->flags & (HA_BLOCK_GAP | HA_BLOCK_DISCONTINUITY | HA_BLOCK_UNKNOWN_GAP)) ++t->gaps;
    t->domain = b->clock_domain; t->epoch = b->clock_epoch;
    t->end = b->first_frame + b->frames; t->lost = b->lost_frames;
    if (b->flags & HA_TIME_HOST_VALID) {
        if (!t->time_seen) { t->first_host = b->host_time_ns; t->first_frame = b->first_frame; }
        t->host = b->host_time_ns;
        if (t->time_seen && b->host_time_ns > t->first_host && b->first_frame > t->first_frame)
            t->observed_ppm = ((double)(b->first_frame - t->first_frame) * 1e9 /
                (double)(b->host_time_ns - t->first_host) / (double)b->sample_rate - 1.0) * 1e6;
        t->time_seen = 1;
    } else t->time_seen = 0;
    t->frames += b->frames; t->seen = 1;
    return HA_OK;
}

ha_result dsp_init(dsp_consumer *d, const ha_stream_info *info)
{
    if (!d || !info || info->quantum_frames > DSP_QUANTUM ||
        info->capture_format.sample_format != HA_PCM_F32 ||
        info->playback_format.sample_format != HA_PCM_F32 ||
        info->capture_format.channels > HA_MAX_CHANNELS ||
        info->playback_format.channels > HA_MAX_CHANNELS ||
        !info->capture_slots || !info->reference_slots) return HA_UNSUPPORTED;
    memset(d, 0, sizeof(*d)); d->format = *info; d->generation = 1;
    d->reference_info.struct_size = sizeof(ha_block_info);
    d->reference_info.struct_version = HA_STRUCT_VERSION;
    return HA_OK;
}

ha_result dsp_pump(dsp_consumer *d, ha_stream *s)
{
    ha_block_info b = HA_INIT(ha_block_info);
    uint32_t n = 0; ha_result r;
    r = ha_stream_read_reference(s, d->reference_pcm, DSP_QUANTUM, &n, &b);
    if (r == HA_OK && n) {
        if (b.generation < d->generation) d->obsolete_reference_frames += n;
        else {
            r = track(&d->render, &b); if (r != HA_OK) return r;
            d->reference_info = b;
            if (b.flags & HA_BLOCK_SILENCE) d->selected_silence_frames += n;
        }
    } else if (r != HA_WOULD_BLOCK && r != HA_BUSY) return r;
    if (d->count == DSP_SLOTS) return HA_OK;
    r = ha_stream_read(s, d->capture_pcm, DSP_QUANTUM, &n, &b);
    if (r == HA_OK && n) {
        uint32_t i; dsp_output *out = &d->output[(d->read + d->count) % DSP_SLOTS];
        r = track(&d->capture, &b); if (r != HA_OK) return r;
        out->info = b;
        /* Intentional mock processing: select channel zero, apply exact gain.
         * Other channels remain available in capture_pcm, with their indices.
         * Output keeps capture rate/time; no fictional resampling or AEC. */
        for (i = 0; i < n; ++i) out->mono[i] = d->capture_pcm[(size_t)i * d->format.capture_format.channels] * 0.5f;
        ++d->count;
    } else if (r != HA_WOULD_BLOCK && r != HA_BUSY) return r;
    return HA_OK;
}

int dsp_take(dsp_consumer *d, dsp_output *out)
{
    if (!d->count) return 0;
    *out = d->output[d->read]; d->read = (d->read + 1u) % DSP_SLOTS; --d->count;
    return 1;
}

void dsp_cancelled(dsp_consumer *d, uint64_t generation)
{
    /* Demonstrates rejecting obsolete selection records after reset. An actual
     * AEC may retain pre-reset reference history to model the acoustic tail. */
    d->generation = generation;
}
