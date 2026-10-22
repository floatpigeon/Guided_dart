#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "drv_pwm.h"
#include "drv_fpioa.h"

int pwm_swing_main(int argc, char **argv);

static const int outputs[] = {1, 2, 3, 5};
static const char *scenario;
static uint64_t virtual_us;
static uint32_t pulses[6];
static unsigned enabled;
static unsigned frequencies;
static unsigned cleanup_mask;
static unsigned extrema[5][4];
static uint32_t pin_configs[72];
static unsigned routed;
static int initialized, deinitialized, failed;

static unsigned channel_bit(int channel)
{
    assert(channel == 1 || channel == 2 || channel == 3 || channel == 5);
    return 1u << channel;
}

static int pin_index(int pin)
{
    const int pins[] = {61, 7, 8, 59};
    for (int i = 0; i < 4; ++i) {
        if (pins[i] == pin)
            return i;
    }
    assert(!"unexpected pin");
    return -1;
}

int drv_fpioa_get_pin_cfg(int pin, uint32_t *value)
{
    const int index = pin_index(pin);
    fpioa_iomux_cfg_t cfg = {.u.value = pin_configs[pin]};
    cfg.u.bit.di = !!(enabled & (1u << outputs[index])) && virtual_us % 3003 < 1520;
    *value = cfg.u.value;
    return 0;
}

int drv_fpioa_set_pin_cfg(int pin, uint32_t value)
{
    pin_index(pin);
    /* The test starts with MSC set and requires every update to preserve it. */
    assert(value & (1u << 9));
    pin_configs[pin] = value;
    return 0;
}

int drv_fpioa_set_pin_func(int pin, fpioa_func_t function)
{
    const int index = pin_index(pin);
    assert(!enabled && !frequencies);
    assert(function == (fpioa_func_t)(PWM0 + outputs[index]));
    if (strcmp(scenario, "pinmux_failure") == 0 && index == 1) {
        failed = 1;
        cleanup_mask = 0;
        return -1;
    }
    pin_configs[pin] = (1u << 9) | (1u << 7) | (7u << 1) | ((pin == 59 ? 3u : 1u) << 11);
    routed |= 1u << index;
    return 0;
}

int drv_fpioa_get_pin_func(int pin, fpioa_func_t *function)
{
    const int index = pin_index(pin);
    assert(routed & (1u << index));
    *function = (fpioa_func_t)(PWM0 + outputs[index]);
    return 0;
}

int drv_pwm_init(void)
{
    initialized = 1;
    return 0;
}

int drv_pwm_deinit(void)
{
    assert(initialized);
    deinitialized = 1;
    return 0;
}

int drv_pwm_set_freq(int channel, uint32_t frequency)
{
    assert(!enabled && !failed && frequency == 333);
    assert(routed == 15u);
    assert(channel == (frequencies ? 3 : 1));
    if (strcmp(scenario, "frequency_failure") == 0 && channel == 3) {
        failed = 1;
        cleanup_mask = 0;
        return -1;
    }
    frequencies++;
    return 0;
}

int drv_pwm_set_duty_ns(int channel, uint32_t pulse)
{
    channel_bit(channel);
    assert(!failed && frequencies == 2);
    assert(pulse >= 1420000 && pulse <= 1620000);
    if (virtual_us == 0)
        assert(!enabled && pulse == 1520000);
    if (virtual_us >= 2000000 && virtual_us < 42000000) {
        const unsigned phase = (unsigned)((virtual_us - 2000000) / 8000000);
        if (phase < 4)
            assert(channel == outputs[phase] || pulse == 1520000);
        for (unsigned i = 0; i < 4; ++i) {
            /* Non-moving channels must actually retain their center pulse. */
            if (phase < 4 && i != phase && outputs[i] != channel)
                assert(pulses[outputs[i]] == 1520000);
            if (outputs[i] == channel) {
                if (pulse == 1420000)
                    extrema[phase][i] |= 1u;
                if (pulse == 1620000)
                    extrema[phase][i] |= 2u;
            }
        }
        if (strcmp(scenario, "pulse_failure") == 0 && virtual_us >= 5000000) {
            failed = 1;
            cleanup_mask = 0;
            return -1;
        }
    }
    pulses[channel] = pulse;
    return 0;
}

int drv_pwm_enable(int channel)
{
    assert(!failed && frequencies == 2);
    for (unsigned i = 0; i < 4; ++i)
        assert(pulses[outputs[i]] == 1520000);
    if (strcmp(scenario, "enable_failure") == 0 && channel == 3) {
        failed = 1;
        cleanup_mask = 0;
        return -1;
    }
    enabled |= channel_bit(channel);
    return 0;
}

int drv_pwm_disable(int channel)
{
    const unsigned bit = channel_bit(channel);
    cleanup_mask |= bit;
    enabled &= ~bit;
    if (virtual_us >= 43000000 && strcmp(scenario, "disable_failure") == 0 && channel == 2)
        return -1;
    return 0;
}

int drv_pwm_get_freq(int channel, uint32_t *frequency)
{
    assert(enabled & channel_bit(channel));
    *frequency = 333;
    return 0;
}

int drv_pwm_get_duty_ns(int channel, uint32_t *pulse)
{
    assert(enabled & channel_bit(channel));
    *pulse = pulses[channel];
    if (strcmp(scenario, "readback_failure") == 0 && channel == 2) {
        failed = 1;
        cleanup_mask = 0;
        return -1;
    }
    return 0;
}

int __wrap_clock_gettime(clockid_t clock, struct timespec *ts)
{
    assert(clock == CLOCK_MONOTONIC);
    ts->tv_sec = (time_t)(virtual_us / 1000000);
    ts->tv_nsec = (long)(virtual_us % 1000000) * 1000;
    return 0;
}

int __wrap_nanosleep(const struct timespec *delay, struct timespec *remaining)
{
    (void)remaining;
    assert(delay->tv_sec >= 0 && delay->tv_nsec >= 0 && delay->tv_nsec < 1000000000);
    if (virtual_us >= 2000000)
        assert(delay->tv_sec == 1 || (delay->tv_sec == 0 && delay->tv_nsec <= 10000000));
    virtual_us += (uint64_t)delay->tv_sec * 1000000 + (uint64_t)delay->tv_nsec / 1000;
    if (strcmp(scenario, "interrupt") == 0 && virtual_us >= 5000000) {
        cleanup_mask = 0;
        raise(SIGTERM);
        errno = EINTR;
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    scenario = argv[1];
    for (unsigned pin = 0; pin < 72; ++pin)
        pin_configs[pin] = 1u << 9;
    char *app_argv[] = {"pwm_swing", NULL};
    const int result = pwm_swing_main(1, app_argv);
    assert(initialized && deinitialized && enabled == 0);
    assert(cleanup_mask == ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 5)));
    if (strcmp(scenario, "success") == 0 || strcmp(scenario, "disable_failure") == 0) {
        assert(result == (strcmp(scenario, "success") == 0 ? 0 : 1));
        assert(virtual_us == 43000000);
        for (unsigned i = 0; i < 4; ++i) {
            assert(pulses[outputs[i]] == 1520000);
            assert(extrema[i][i] == 3u && extrema[4][i] == 3u);
        }
    } else if (strcmp(scenario, "interrupt") == 0) {
        assert(result == 128 + SIGTERM && virtual_us == 5000000);
    } else {
        assert(failed && result == 1);
    }
    return 0;
}
