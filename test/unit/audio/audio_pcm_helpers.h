/*
 * audio_pcm_helpers.h -- synthetic PCM generators and measurements shared by the audio unit tests.
 * Pure test-side helpers (signal generation, RMS, zero-crossing frequency); no production code.
 */
#ifndef AUDIO_PCM_HELPERS_H
#define AUDIO_PCM_HELPERS_H

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Interleaved 16-bit sine; channel c uses freq[c] (or freq[0] if only one frequency is given). */
static short *pcm_sine(int frames, int channels, int sample_rate, const double *freq, int nfreq, double amp)
{
    short *p = malloc((size_t)frames * channels * sizeof(short));
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < channels; c++) {
            double f = freq[c < nfreq ? c : nfreq - 1];
            p[i * channels + c] = (short)lrint(amp * sin(2.0 * M_PI * f * i / sample_rate));
        }
    return p;
}

static short *pcm_sine1(int frames, int sample_rate, double freq, double amp)
{
    return pcm_sine(frames, 1, sample_rate, &freq, 1, amp);
}

/* RMS of channel `ch` of an interleaved buffer, skipping `skip` frames at each end. */
static double pcm_rms(const short *p, int frames, int channels, int ch, int skip)
{
    double acc = 0;
    int n = 0;
    for (int i = skip; i < frames - skip; i++, n++) {
        double v = p[i * channels + ch];
        acc += v * v;
    }
    return n ? sqrt(acc / n) : 0.0;
}

/* Frequency of channel `ch` from rising zero crossings (Hz); 0 if fewer than 3 crossings. */
static double pcm_freq(const short *p, int frames, int channels, int ch, int sample_rate)
{
    int first = -1, last = -1, count = 0;
    for (int i = 1; i < frames; i++) {
        if (p[(i - 1) * channels + ch] < 0 && p[i * channels + ch] >= 0) {
            if (first < 0) first = i;
            last = i;
            count++;
        }
    }
    if (count < 3) return 0.0;
    return (double)(count - 1) * sample_rate / (double)(last - first);
}

static int pcm_peak(const short *p, int n)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        int v = p[i] < 0 ? -p[i] : p[i];
        if (v > m) m = v;
    }
    return m;
}

#endif
