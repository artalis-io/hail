#ifndef DSP_CONSUMER_H
#define DSP_CONSUMER_H
#include <hail.h>

/* Example-owned storage and policy, independent of Hail's implementation.
 * A real DSP library supplies its own processing, alignment and resampler. */
#define DSP_QUANTUM 128u
#define DSP_SLOTS 2u
typedef struct dsp_timeline {
    uint64_t domain, epoch, end, host, first_host, first_frame;
    uint64_t frames, gaps, resets, lost;
    uint32_t seen, time_seen;
    double observed_ppm;
} dsp_timeline;
typedef struct dsp_output {
    ha_block_info info;
    float mono[DSP_QUANTUM];
} dsp_output;
typedef struct dsp_consumer {
    ha_stream_info format;
    dsp_timeline capture, render;
    float capture_pcm[DSP_QUANTUM * HA_MAX_CHANNELS];
    float reference_pcm[DSP_QUANTUM * HA_MAX_CHANNELS];
    ha_block_info reference_info;
    dsp_output output[DSP_SLOTS];
    uint32_t read, count;
    uint64_t generation, obsolete_reference_frames, selected_silence_frames;
} dsp_consumer;

ha_result dsp_init(dsp_consumer *, const ha_stream_info *);
/* One application I/O owner; bounded to one reference and one capture read.
 * Full processed-output storage leaves capture queued: Hail owns drop-newest. */
ha_result dsp_pump(dsp_consumer *, ha_stream *);
int dsp_take(dsp_consumer *, dsp_output *);
void dsp_cancelled(dsp_consumer *, uint64_t generation);
#endif
