/* SPDX-License-Identifier: MIT */
/* Fixed ALSA ring, adaptive target fill. Silence unless --tone is given.
 * Build on Linux with libasound development headers:
 *   cc -std=c11 -O2 -Wall -Wextra adaptive_pcm.c -lasound -lm -o adaptive_pcm
 * Policy-only build with no ALSA dependency:
 *   cc -std=c11 -O2 -Wall -Wextra -DPOLICY_ONLY adaptive_pcm.c -o depth_test
 */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define STABLE_NS UINT64_C(30000000000)

struct depth {
    unsigned target, floor, ceiling;
    uint64_t stable_since;
};

static struct depth initial_depth(unsigned ring_periods)
{
    unsigned floor = ring_periods > 4 ? 4 : ring_periods - 1;
    return (struct depth){floor, floor, ring_periods, 0};
}

static void underrun(struct depth *depth)
{
    unsigned room = depth->ceiling - depth->target;
    depth->target += room < 2 ? room : 2;
    depth->stable_since = 0;
}

static void wrote_period(struct depth *depth, uint64_t now)
{
    if (depth->stable_since == 0)
        depth->stable_since = now;
    if (depth->target > depth->floor &&
        now - depth->stable_since >= STABLE_NS) {
        --depth->target;
        depth->stable_since = now;
    }
}

static int self_test(void)
{
    struct depth depth = initial_depth(8);
    if (depth.target != 4 || initial_depth(3).target != 2)
        return 1;
    underrun(&depth);
    if (depth.target != 6)
        return 1;
    underrun(&depth);
    underrun(&depth);
    if (depth.target != 8)
        return 1;
    wrote_period(&depth, 1);
    wrote_period(&depth, STABLE_NS);
    if (depth.target != 8)
        return 1;
    wrote_period(&depth, STABLE_NS + 1);
    if (depth.target != 7)
        return 1;
    for (unsigned i = 2; i <= 6; ++i)
        wrote_period(&depth, STABLE_NS * i + 1);
    if (depth.target != 4)
        return 1;
    puts("Depth policy passed");
    return 0;
}

#ifdef POLICY_ONLY
int main(void)
{
    return self_test();
}
#else
#include <alloca.h>
#include <alsa/asoundlib.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

struct geometry {
    unsigned rate;
    snd_pcm_uframes_t period, capacity;
};

struct playback {
    struct depth depth;
    uint64_t submitted, observed, queued_sum;
    unsigned xruns, peak_target;
};

static uint64_t monotonic_ns(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
        perror("clock_gettime");
        exit(1);
    }
    return (uint64_t)time.tv_sec * UINT64_C(1000000000) + time.tv_nsec;
}

#define CHECK_ALSA(call) do { int ret = (call); if (ret < 0) return ret; } while (0)

static int configure(snd_pcm_t *pcm, struct geometry *g)
{
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
    snd_pcm_uframes_t boundary;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_sw_params_alloca(&sw);

    CHECK_ALSA(snd_pcm_hw_params_any(pcm, hw));
    CHECK_ALSA(snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED));
    CHECK_ALSA(snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16));
    CHECK_ALSA(snd_pcm_hw_params_set_channels(pcm, hw, 2));
    CHECK_ALSA(snd_pcm_hw_params_set_rate_near(pcm, hw, &g->rate, NULL));
    CHECK_ALSA(snd_pcm_hw_params_set_period_size_near(pcm, hw, &g->period, NULL));
    CHECK_ALSA(snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &g->capacity));
    CHECK_ALSA(snd_pcm_hw_params(pcm, hw));
    CHECK_ALSA(snd_pcm_hw_params_current(pcm, hw));
    CHECK_ALSA(snd_pcm_hw_params_get_rate(hw, &g->rate, NULL));
    CHECK_ALSA(snd_pcm_get_params(pcm, &g->capacity, &g->period));
    if (g->rate == 0 || g->period == 0 || g->period > 4096 ||
        g->capacity > 65536 || g->capacity / g->period < 3)
        return -EINVAL;

    CHECK_ALSA(snd_pcm_sw_params_current(pcm, sw));
    CHECK_ALSA(snd_pcm_sw_params_get_boundary(sw, &boundary));
    CHECK_ALSA(snd_pcm_sw_params_set_start_threshold(pcm, sw, boundary));
    CHECK_ALSA(snd_pcm_sw_params_set_stop_threshold(pcm, sw, g->capacity));
    CHECK_ALSA(snd_pcm_sw_params(pcm, sw));
    return snd_pcm_prepare(pcm);
}

#undef CHECK_ALSA

static void wait_for_space(snd_pcm_uframes_t frames, unsigned rate)
{
    /* A period wake-up can be later than the point where the target permits
       another write. Keep this simple timer bounded and verify it on hardware. */
    uint64_t ns = frames * UINT64_C(1000000000) / rate;
    if (ns < 50000)
        ns = 50000;
    if (ns > 500000)
        ns = 500000;
    nanosleep(&(struct timespec){0, (long)ns}, NULL);
}

static void render_period(int16_t *samples, const struct geometry *g,
                          uint64_t cursor, uint64_t fade_from, bool tone)
{
    if (!tone)
        return;
    const double tau = 6.283185307179586;
    for (snd_pcm_uframes_t i = 0; i < g->period; ++i) {
        uint64_t frame = cursor + i;
        double fade = fmin(1, 200.0 * (double)(frame - fade_from) / g->rate);
        int16_t sample = (int16_t)(512.0 * fade *
                                   sin(tau * 440.0 * (double)frame / g->rate));
        samples[2 * i] = samples[2 * i + 1] = sample;
    }
}

static int stream(snd_pcm_t *pcm, const struct geometry *g, int16_t *samples,
                  struct playback *stats, unsigned seconds, bool tone)
{
    uint64_t end = monotonic_ns() + seconds * UINT64_C(1000000000);
    uint64_t cursor = 0, fade_from = 0;
    snd_pcm_uframes_t pending = 0, primed = 0;
    bool running = false, started = false;

    while (monotonic_ns() < end) {
        snd_pcm_sframes_t result = snd_pcm_avail(pcm);
        if (result >= 0) {
            snd_pcm_uframes_t free = (snd_pcm_uframes_t)result;
            if (free > g->capacity)
                return -EIO;
            snd_pcm_uframes_t remaining = pending ? pending : g->period;
            snd_pcm_uframes_t target = stats->depth.target * g->period;
            snd_pcm_uframes_t queued = g->capacity - free;
            if (queued > target - remaining) {
                wait_for_space(queued - (target - remaining), g->rate);
                continue;
            }
            if (!pending) {
                render_period(samples, g, cursor, fade_from, tone);
                pending = g->period;
            }
            result = snd_pcm_writei(pcm, samples + 2 * (g->period - pending), pending);
            if (result > 0) {
                pending -= (snd_pcm_uframes_t)result;
                cursor += (uint64_t)result;
                stats->submitted += (uint64_t)result;
                if (running) {
                    if (!pending) {
                        stats->queued_sum += queued;
                        ++stats->observed;
                        wrote_period(&stats->depth, monotonic_ns());
                    }
                    continue;
                }
                primed += (snd_pcm_uframes_t)result;
                if (primed < target)
                    continue;
                result = snd_pcm_start(pcm);
                if (result == 0) {
                    running = true;
                    started = true;
                    continue;
                }
            }
        }

        if (result == 0 || result == -EAGAIN || result == -EINTR) {
            wait_for_space(1, g->rate);
            continue;
        }
        if (result != -EPIPE)
            return (int)result;
        if (++stats->xruns > 64)
            return -EPIPE;
        underrun(&stats->depth);
        if (stats->depth.target > stats->peak_target)
            stats->peak_target = stats->depth.target;
        result = snd_pcm_prepare(pcm);
        if (result < 0)
            return (int)result;
        /* Old PCM is gone. Re-prime the same handle and ramp the signal in. */
        pending = primed = 0;
        fade_from = cursor;
        running = false;
    }
    return started ? 0 : -ETIMEDOUT;
}

static unsigned arg_number(const char *text, unsigned min, unsigned max)
{
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (text == end || *end || errno || value < min || value > max) {
        fprintf(stderr, "Invalid number %s (expected %u-%u)\n", text, min, max);
        exit(2);
    }
    return (unsigned)value;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
        return self_test();
    bool tone = argc > 2 && strcmp(argv[argc - 1], "--tone") == 0;
    argc -= tone;
    if (argc < 2 || argc > 6 || argv[1][0] == '-') {
        fprintf(stderr, "Usage: %s DEVICE [SECONDS [RATE [PERIOD [CAPACITY]]]] [--tone]\n"
                        "       %s --self-test\n", argv[0], argv[0]);
        return 2;
    }
    unsigned seconds = argc > 2 ? arg_number(argv[2], 1, 300) : 10;
    struct geometry g = {
        argc > 3 ? arg_number(argv[3], 8000, 192000) : 48000,
        argc > 4 ? arg_number(argv[4], 16, 4096) : 64,
        argc > 5 ? arg_number(argv[5], 48, 65536) : 512,
    };
    snd_pcm_t *pcm = NULL;
    int result = snd_pcm_open(&pcm, argv[1], SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (result >= 0)
        result = configure(pcm, &g);
    if (result >= 0) {
        int16_t *samples = calloc(2 * g.period, sizeof(*samples));
        if (!samples) {
            result = -ENOMEM;
        } else {
            struct playback stats = {.depth = initial_depth((unsigned)(g.capacity / g.period))};
            stats.peak_target = stats.depth.target;
            printf("PCM %u Hz, period %lu frames, ring %lu frames, initial fill %lu frames\n",
                   g.rate, g.period, g.capacity, stats.depth.target * g.period);
            result = stream(pcm, &g, samples, &stats, seconds, tone);
            printf("XRUNs %u, peak/final fill %lu/%lu frames, submitted %llu frames\n",
                   stats.xruns, stats.peak_target * g.period, stats.depth.target * g.period,
                   (unsigned long long)stats.submitted);
            if (stats.observed)
                printf("Mean queued ring frames before a completed running write %.1f\n",
                       (double)stats.queued_sum / stats.observed);
            snd_pcm_drop(pcm);
            free(samples);
        }
    }
    if (pcm)
        snd_pcm_close(pcm);
    if (result < 0)
        fprintf(stderr, "ALSA playback error: %s\n", snd_strerror(result));
    return result < 0;
}
#endif
