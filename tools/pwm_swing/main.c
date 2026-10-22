#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "drv_pwm.h"
#include "drv_fpioa.h"

enum {
    CHANNEL_COUNT = 4,
    FREQUENCY_HZ = 333,
    CENTER_US = 1520,
    AMPLITUDE_US = 100,
    UPDATE_US = 10000,
    CYCLE_US = 4000000,
    SWING_US = 2 * CYCLE_US
};

/* Dart V1 servo connectors, in test order; PWM0 and PWM4 are unused. */
static const int channels[CHANNEL_COUNT] = {1, 2, 3, 5};
static const int pins[CHANNEL_COUNT] = {61, 7, 8, 59};
static const fpioa_func_t functions[CHANNEL_COUNT] = {PWM1, PWM2, PWM3, PWM5};
static volatile sig_atomic_t stop_signal;

static void request_stop(int signum)
{
    stop_signal = signum;
}

static int now_us(uint64_t *value)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        return -1;
    }
    *value = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
    return 0;
}

static int wait_until(uint64_t deadline)
{
    while (!stop_signal) {
        uint64_t now;
        if (now_us(&now) != 0)
            return -1;
        if (now >= deadline)
            return 0;
        const uint64_t remaining = deadline - now;
        struct timespec delay = {
            .tv_sec = (time_t)(remaining / 1000000u),
            .tv_nsec = (long)((remaining % 1000000u) * 1000u)
        };
        if (nanosleep(&delay, NULL) != 0 && errno != EINTR) {
            perror("nanosleep");
            return -1;
        }
    }
    return -1;
}

static int hold_us(uint64_t duration)
{
    uint64_t start;
    return now_us(&start) == 0 ? wait_until(start + duration) : -1;
}

static int set_pulse(int index, uint32_t pulse_us)
{
    if (stop_signal)
        return -1;
    if (drv_pwm_set_duty_ns(channels[index], pulse_us * 1000u) != 0) {
        fprintf(stderr, "PWM%d: failed to set pulse to %" PRIu32 " us\n",
                channels[index], pulse_us);
        return -1;
    }
    return 0;
}

static int center_all(void)
{
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        if (set_pulse(i, CENTER_US) != 0)
            return -1;
    }
    return 0;
}

static int disable_all(void)
{
    int result = 0;
    /* Attempt every connector even when a previous disable fails. */
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        if (drv_pwm_disable(channels[i]) != 0) {
            fprintf(stderr, "PWM%d: failed to disable output\n", channels[i]);
            result = -1;
        }
    }
    return result;
}

static int configure_pin_routes(void)
{
    puts("Configuring PWM pin routes (bank voltage selection is preserved):");
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        fpioa_iomux_cfg_t before, after;
        fpioa_func_t function;
        if (stop_signal || drv_fpioa_get_pin_cfg(pins[i], &before.u.value) != 0) {
            fprintf(stderr, "IO%d: failed to read pin configuration\n", pins[i]);
            return -1;
        }
        printf("  IO%d before: cfg=0x%08" PRIx32 ", sel=%u, oe=%u, ie=%u\n",
               pins[i], before.u.value, (unsigned)before.u.bit.io_sel,
               (unsigned)before.u.bit.oe, (unsigned)before.u.bit.ie);
        /* Explicit routing also enables the output driver. Enable the input
         * buffer alongside PWM so DI can sample the physical pad level. */
        if (drv_fpioa_set_pin_func(pins[i], functions[i]) != 0 ||
            drv_fpioa_set_pin_ie(pins[i], 1) != 0 ||
            drv_fpioa_get_pin_func(pins[i], &function) != 0 ||
            drv_fpioa_get_pin_cfg(pins[i], &after.u.value) != 0) {
            fprintf(stderr, "IO%d: failed to configure PWM%d pin route\n", pins[i], channels[i]);
            return -1;
        }
        if (function != functions[i] || !after.u.bit.oe || !after.u.bit.ie ||
            after.u.bit.msc != before.u.bit.msc) {
            fprintf(stderr, "IO%d: pin configuration readback mismatch\n", pins[i]);
            return -1;
        }
        printf("  IO%d after: PWM%d, cfg=0x%08" PRIx32 ", sel=%u, oe=1, ie=1\n",
               pins[i], channels[i], after.u.value, (unsigned)after.u.bit.io_sel);
    }
    return 0;
}

static int configure_outputs(void)
{
    if (disable_all() != 0 || stop_signal || configure_pin_routes() != 0)
        return -1;

    /* PWM0/1/2 share a period, as do PWM3/4/5. Configure both groups
     * before setting any pulse or enabling any connector. */
    for (int group = 0; group < 2; ++group) {
        const int channel = group == 0 ? 1 : 3;
        if (stop_signal || drv_pwm_set_freq(channel, FREQUENCY_HZ) != 0) {
            fprintf(stderr, "PWM%d group: failed to set frequency\n", channel);
            return -1;
        }
    }
    if (center_all() != 0)
        return -1;
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        if (stop_signal || drv_pwm_enable(channels[i]) != 0) {
            fprintf(stderr, "PWM%d: failed to enable output\n", channels[i]);
            return -1;
        }
    }

    puts("Driver configuration readback (verify the waveform at the connector):");
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        uint32_t frequency = 0, pulse_ns = 0;
        if (drv_pwm_get_freq(channels[i], &frequency) != 0 ||
            drv_pwm_get_duty_ns(channels[i], &pulse_ns) != 0) {
            fprintf(stderr, "PWM%d: configuration readback failed\n", channels[i]);
            return -1;
        }
        printf("  CH%d: PWM%d / IO%d, %" PRIu32 " Hz, %" PRIu32 " ns\n",
               i + 1, channels[i], pins[i], frequency, pulse_ns);
        if (frequency != FREQUENCY_HZ ||
            pulse_ns < CENTER_US * 1000u - 1000u ||
            pulse_ns > CENTER_US * 1000u + 1000u) {
            fprintf(stderr, "PWM%d: unexpected configuration\n", channels[i]);
            return -1;
        }
    }
    return 0;
}

static int hold_center_and_sample_pads(void)
{
    unsigned high[CHANNEL_COUNT] = {0}, low[CHANNEL_COUNT] = {0};
    uint64_t start;
    if (now_us(&start) != 0)
        return -1;
    /* Sparse samples only check whether both levels reach each physical pad;
     * they do not measure pulse width, period or connector wiring. */
    for (unsigned sample = 0; sample < 200; ++sample) {
        if (wait_until(start + (uint64_t)sample * UPDATE_US) != 0)
            return -1;
        for (int i = 0; i < CHANNEL_COUNT; ++i) {
            fpioa_iomux_cfg_t cfg;
            if (drv_fpioa_get_pin_cfg(pins[i], &cfg.u.value) != 0) {
                fprintf(stderr, "IO%d: failed to sample pad input\n", pins[i]);
                return -1;
            }
            if (cfg.u.bit.di)
                ++high[i];
            else
                ++low[i];
        }
    }
    if (wait_until(start + 2000000u) != 0)
        return -1;
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        printf("  IO%d pad samples: high=%u, low=%u, %s\n", pins[i], high[i], low[i],
               high[i] && low[i] ? "both levels observed" : "WARNING: no transitions confirmed");
    }
    return 0;
}

/* selected == -1 moves all four connectors; otherwise only that index moves.
 * Start and finish at center, with two complete 4-second sine cycles. */
static int swing(int selected)
{
    uint64_t start;
    if (now_us(&start) != 0)
        return -1;
    uint64_t next_log = 0;
    for (;;) {
        uint64_t now;
        if (stop_signal || now_us(&now) != 0)
            return -1;
        uint64_t elapsed = now - start;
        if (elapsed > SWING_US)
            elapsed = SWING_US;
        const double phase = 6.2831853071795864769 *
                             (double)(elapsed % CYCLE_US) / CYCLE_US;
        const uint32_t pulse = (uint32_t)lround(CENTER_US + AMPLITUDE_US * sin(phase));
        for (int i = 0; i < CHANNEL_COUNT; ++i) {
            if ((selected < 0 || selected == i) && set_pulse(i, pulse) != 0)
                return -1;
        }
        if (elapsed >= next_log) {
            printf("  t=%" PRIu64 " ms, target=%" PRIu32 " us\n", elapsed / 1000u, pulse);
            next_log = elapsed + 1000000u;
        }
        if (elapsed == SWING_US)
            return 0;
        uint64_t next = (elapsed / UPDATE_US + 1u) * UPDATE_US;
        if (next > SWING_US)
            next = SWING_US;
        if (wait_until(start + next) != 0)
            return -1;
    }
}

static int run_sequence(void)
{
    puts("[1/4] Hold all connectors at 1520 us for 2 seconds");
    if (hold_center_and_sample_pads() != 0)
        return -1;
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        printf("[2/4] CH%d / PWM%d / IO%d: two cycles, 1420..1620 us\n",
               i + 1, channels[i], pins[i]);
        if (swing(i) != 0)
            return -1;
    }
    puts("[3/4] All four connectors: two cycles, 1420..1620 us");
    if (swing(-1) != 0 || center_all() != 0)
        return -1;
    puts("[4/4] Hold all connectors at 1520 us for 1 second, then disable");
    return hold_us(1000000u);
}

int main(int argc, char **argv)
{
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        puts("Usage: pwm_swing\n"
             "PTK 7350 MG-D / Dart V1: PWM1/2/3/5 on IO61/7/8/59.\n"
             "333 Hz, center 1520 us, sine swing 1420..1620 us; about 43 seconds.\n"
             "Run unloaded with a 4.8..8.4 V servo supply and a common ground.\n"
             "SIGINT/SIGTERM disable all four outputs.");
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "Usage: %s [--help]\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction action = {0};
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0) {
        perror("sigaction");
        return 1;
    }
    puts("PTK 7350 MG-D PWM test: 333 Hz, center 1520 us, amplitude 100 us");
    if (drv_pwm_init() != 0)
        return 1;

    int result = configure_outputs();
    if (result == 0)
        result = run_sequence();
    if (disable_all() != 0)
        result = -1;
    if (drv_pwm_deinit() != 0)
        result = -1;
    if (stop_signal) {
        fprintf(stderr, "Stopped by signal %d; output disable attempted\n", (int)stop_signal);
        return 128 + stop_signal;
    }
    puts(result == 0 ? "PWM test complete; all four outputs disabled" : "PWM test failed");
    return result == 0 ? 0 : 1;
}
