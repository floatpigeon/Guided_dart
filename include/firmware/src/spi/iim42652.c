#define _POSIX_C_SOURCE 200809L

#include "spi/iim42652.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "drv_spi.h"

#define IIM42652_REG_DEVICE_CONFIG        0x11u
#define IIM42652_REG_TEMP_DATA1           0x1du
#define IIM42652_REG_WHO_AM_I             0x75u
#define IIM42652_REG_PWR_MGMT0            0x4eu
#define IIM42652_REG_GYRO_CONFIG0         0x4fu
#define IIM42652_REG_ACCEL_CONFIG0        0x50u
#define IIM42652_REG_BANK_SEL             0x76u
#define IIM42652_REG_INT_STATUS           0x2du
#define IIM42652_SOFT_RESET               0x01u
#define IIM42652_PWR_GYRO_ACCEL_LOW_NOISE 0x0fu
#define IIM42652_DEFAULT_ODR              0x06u
#define IIM42652_GRAVITY_M_S2             9.80665f
#define IIM42652_DEG_TO_RAD               0.01745329251994329577f

struct dart_iim42652 {
    dart_iim42652_config_t config;
    drv_spi_inst_t spi;
    float accelerometer_lsb_per_g;
    float gyroscope_lsb_per_dps;
};

const char* dart_iim42652_device_name(uint8_t device_id) {
    switch (device_id) {
    case DART_IMU_DEVICE_ID_IIM42652: return "IIM-42652";
    default: return "unknown";
    }
}

const char* dart_iim42652_stage_name(dart_iim42652_init_stage_t stage) {
    static const char* names[] = {
        "bus", "identify", "reset", "configure", "enable", "verify", "wait_data", "ready"};
    return (unsigned int)stage < sizeof(names) / sizeof(names[0]) ? names[stage] : "unknown";
}

static int delay_ms(long milliseconds) {
    struct timespec remaining = {
        .tv_sec = milliseconds / 1000, .tv_nsec = (milliseconds % 1000) * 1000000L};
    while (nanosleep(&remaining, &remaining) != 0)
        if (errno != EINTR)
            return -1;
    return 0;
}

static int valid_config(const dart_iim42652_config_t* config) {
    unsigned int index;
    if (config == NULL || config->bus < 0 || config->bus > 2 || config->bus_hz == 0u
        || config->timeout_ms == 0u || config->chip_select_pin < -1 || config->chip_select_pin > 63
        || config->accelerometer_range > DART_IIM42652_ACCEL_2G
        || config->gyroscope_range > DART_IIM42652_GYRO_15_625DPS
        || config->output_data_rate > 0x0fu || config->pre_access_delay_ms > 10000u
        || config->post_enable_delay_ms > 10000u || config->ready_timeout_ms > 10000u
        || config->skip_device_id_check > 1u
        || (config->post_enable_delay_ms != 0u && config->post_enable_delay_ms < 50u))
        return 0;
    for (index = 0; index < 3; ++index)
        if (!isfinite(config->gyroscope_bias_rad_s[index]))
            return 0;
    return 1;
}

static float accelerometer_sensitivity(uint8_t range) {
    static const float values[] = {2048.0f, 4096.0f, 8192.0f, 16384.0f};
    return values[range];
}

static float gyroscope_sensitivity(uint8_t range) {
    static const float values[] = {16.4f, 32.8f, 65.5f, 131.0f, 262.0f, 524.3f, 1048.6f, 2097.2f};
    return values[range];
}

static int write_register(dart_iim42652_t* sensor, uint8_t reg, uint8_t value) {
    const uint8_t bytes[2] = {(uint8_t)(reg & 0x7fu), value};
    if (sensor == NULL || sensor->spi == NULL)
        return -1;
    return drv_spi_write(sensor->spi, bytes, sizeof(bytes), true) == 2 ? 0 : -1;
}

static int read_registers(dart_iim42652_t* sensor, uint8_t reg, uint8_t* buffer, uint16_t length) {
    uint8_t tx[15];
    uint8_t rx[15];
    size_t transfer_length;
    if (sensor == NULL || sensor->spi == NULL || buffer == NULL || length == 0u || length > 14u)
        return -1;
    transfer_length = (size_t)length + 1u;
    memset(tx, 0, sizeof(tx));
    memset(rx, 0, sizeof(rx));
    tx[0] = (uint8_t)(reg | 0x80u);
    if (drv_spi_transfer(sensor->spi, tx, rx, transfer_length, true) != (int)transfer_length) {
        return -1;
    }
    memcpy(buffer, &rx[1], length);
    return 0;
}

static int16_t signed_word(uint8_t high, uint8_t low) {
    uint16_t bits = (uint16_t)(((uint16_t)high << 8) | low);
    int16_t value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint64_t monotonic_time_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 || now.tv_nsec < 0)
        return 0u;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

dart_iim42652_status_t
    dart_iim42652_init(const dart_iim42652_config_t* config, dart_iim42652_t** out_sensor) {
    return dart_iim42652_init_with_diagnostic(config, out_sensor, NULL);
}

static int read_status(dart_iim42652_t* sensor, dart_iim42652_diagnostic_t* diagnostic) {
    if (read_registers(sensor, IIM42652_REG_INT_STATUS, &diagnostic->interrupt_status, 1u) != 0)
        return -1;
    diagnostic->interrupt_status_seen |= diagnostic->interrupt_status;
    return 0;
}

static int valid_axes(const int16_t values[3]) {
    /* DS-000440: with FIFO_HOLD_LAST_DATA_EN=0, -32768 means no valid data.
     * DATA_RDY alone can come from the accelerometer while the gyro is invalid. */
    return values[0] != INT16_MIN && values[1] != INT16_MIN && values[2] != INT16_MIN;
}

static void decode_last_axes(dart_iim42652_diagnostic_t* diagnostic, const uint8_t bytes[14]) {
    for (unsigned int i = 0; i < 3u; ++i) {
        diagnostic->last_accelerometer[i] = signed_word(bytes[2u + i * 2u], bytes[3u + i * 2u]);
        diagnostic->last_gyroscope[i] = signed_word(bytes[8u + i * 2u], bytes[9u + i * 2u]);
    }
}

static void save_raw_probe(dart_iim42652_diagnostic_t* diagnostic, const uint8_t bytes[14]) {
    memcpy(diagnostic->raw_probe, bytes, sizeof(diagnostic->raw_probe));
    diagnostic->raw_probe_read = 1;
    diagnostic->raw_probe_stage = diagnostic->stage;
    decode_last_axes(diagnostic, bytes);
}

static void capture_raw_probe(dart_iim42652_t* sensor, dart_iim42652_diagnostic_t* diagnostic) {
    uint8_t bytes[14];
    /* Retain the last successful snapshot if this optional read fails. */
    if (read_registers(sensor, IIM42652_REG_TEMP_DATA1, bytes, sizeof(bytes)) == 0)
        save_raw_probe(diagnostic, bytes);
}

static int plausible_raw_frame(const uint8_t bytes[14]) {
    int all_zero = 1, all_one = 1;
    for (unsigned int i = 0; i < 14u; ++i) {
        all_zero &= bytes[i] == 0u;
        all_one &= bytes[i] == 0xffu;
    }
    return !all_zero && !all_one;
}

dart_iim42652_status_t dart_iim42652_init_with_diagnostic(
    const dart_iim42652_config_t* config, dart_iim42652_t** out_sensor,
    dart_iim42652_diagnostic_t* diagnostic) {
    dart_iim42652_diagnostic_t local;
    dart_iim42652_t* sensor = NULL;
    dart_iim42652_status_t result = DART_IIM42652_BUS_ERROR;
    uint8_t device_id = 0, reset_id = 0, interfaces[2], applied[3], odr;
    uint64_t deadline;
    if (diagnostic == NULL)
        diagnostic = &local;
    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->expected_device_id = DART_IMU_DEVICE_ID_IIM42652;
    if (out_sensor == NULL || !valid_config(config)) {
        diagnostic->result = DART_IIM42652_INVALID_ARGUMENT;
        return diagnostic->result;
    }
    if (*out_sensor != NULL) {
        diagnostic->result = DART_IIM42652_ALREADY_INITIALIZED;
        return diagnostic->result;
    }
    diagnostic->device_id_check_skipped = config->skip_device_id_check;
    diagnostic->started_ns = monotonic_time_ns();
    if (diagnostic->started_ns == 0u) {
        diagnostic->result = DART_IIM42652_CLOCK_ERROR;
        return diagnostic->result;
    }
    sensor = (dart_iim42652_t*)calloc(1u, sizeof(*sensor));
    if (sensor == NULL) {
        result = DART_IIM42652_NO_MEMORY;
        goto finished;
    }
    sensor->config = *config;
    if (drv_spi_inst_create(
            config->bus, true, SPI_HAL_MODE_3, config->bus_hz, 8u, config->chip_select_pin,
            SPI_HAL_DATA_LINE_1, &sensor->spi) != 0) {
        /* SDK frees its partial instance but leaves a dangling output pointer. */
        sensor->spi = NULL;
        goto finished;
    }
    diagnostic->stage = DART_IIM42652_STAGE_IDENTIFY;
    if (delay_ms(config->pre_access_delay_ms) != 0)
        goto clock_error;
    diagnostic->first_access_ns = monotonic_time_ns();
    if (diagnostic->first_access_ns == 0u)
        goto clock_error;
    if (write_register(sensor, IIM42652_REG_BANK_SEL, 0u) != 0)
        goto finished;
    if (config->skip_device_id_check)
        capture_raw_probe(sensor, diagnostic);
    if (read_registers(sensor, IIM42652_REG_WHO_AM_I, &device_id, 1u) != 0)
        goto finished;
    diagnostic->device_id_read = 1;
    diagnostic->observed_device_id = device_id;
    if (!config->skip_device_id_check && device_id != DART_IMU_DEVICE_ID_IIM42652) {
        result = DART_IIM42652_UNEXPECTED_DEVICE_ID;
        goto finished;
    }
    diagnostic->stage = DART_IIM42652_STAGE_RESET;
    if (write_register(sensor, IIM42652_REG_DEVICE_CONFIG, IIM42652_SOFT_RESET) != 0)
        goto finished;
    diagnostic->reset_ns = monotonic_time_ns();
    if (diagnostic->reset_ns == 0u || delay_ms(2) != 0)
        goto clock_error;
    /* Soft reset requires >=1 ms without any register accesses. */
    if (read_registers(sensor, IIM42652_REG_WHO_AM_I, &reset_id, 1u) != 0)
        goto finished;
    diagnostic->reset_device_id_read = 1;
    diagnostic->reset_device_id = reset_id;
    if (read_status(sensor, diagnostic) != 0)
        goto finished;
    if (!config->skip_device_id_check && reset_id != device_id) {
        result = DART_IIM42652_UNEXPECTED_DEVICE_ID;
        goto finished;
    }
    diagnostic->stage = DART_IIM42652_STAGE_CONFIGURE;
    if (write_register(sensor, IIM42652_REG_BANK_SEL, 1u) != 0)
        goto finished;
    const int axis_read = read_registers(sensor, 0x03u, &diagnostic->sensor_config0, 1u);
    /* Restore Bank 0 even when the diagnostic read fails. */
    const int bank_restored = write_register(sensor, IIM42652_REG_BANK_SEL, 0u);
    if (axis_read != 0 || bank_restored != 0)
        goto finished;
    diagnostic->sensor_config_read = 1;
    if ((diagnostic->sensor_config0 & 0x38u) != 0u) {
        result = DART_IIM42652_CONFIGURATION_MISMATCH;
        goto finished;
    }
    odr = config->output_data_rate == 0u ? IIM42652_DEFAULT_ODR : config->output_data_rate;
    /* Configure while asleep. Preserve variant-specific reserved bits. */
    if (write_register(
            sensor, IIM42652_REG_GYRO_CONFIG0, (uint8_t)((config->gyroscope_range << 5) | odr)) != 0
        || write_register(
            sensor, IIM42652_REG_ACCEL_CONFIG0,
            (uint8_t)((config->accelerometer_range << 5) | odr)) != 0)
        goto finished;
    diagnostic->configured_ns = monotonic_time_ns();
    if (diagnostic->configured_ns == 0u)
        goto clock_error;
    diagnostic->stage = DART_IIM42652_STAGE_ENABLE;
    if (write_register(sensor, IIM42652_REG_PWR_MGMT0, IIM42652_PWR_GYRO_ACCEL_LOW_NOISE) != 0)
        goto finished;
    diagnostic->enabled_ns = monotonic_time_ns();
    /* No writes for >=200 us after OFF->ON. Typical gyro startup is 30 ms;
     * the separate 45 ms requirement is minimum ON duration, not startup time. */
    if (diagnostic->enabled_ns == 0u
        || delay_ms(config->post_enable_delay_ms == 0u ? 50u : config->post_enable_delay_ms) != 0)
        goto clock_error;
    diagnostic->stage = DART_IIM42652_STAGE_VERIFY;
    if (config->skip_device_id_check)
        capture_raw_probe(sensor, diagnostic);
    if (read_registers(sensor, IIM42652_REG_PWR_MGMT0, applied, 3u) != 0
        || read_registers(sensor, 0x4cu, interfaces, 2u) != 0)
        goto finished;
    diagnostic->registers_read = 1;
    diagnostic->power_config = applied[0];
    diagnostic->gyro_config = applied[1];
    diagnostic->accel_config = applied[2];
    diagnostic->interface_config0 = interfaces[0];
    diagnostic->interface_config1 = interfaces[1];
    if (applied[0] != IIM42652_PWR_GYRO_ACCEL_LOW_NOISE
        || applied[1] != (uint8_t)((config->gyroscope_range << 5) | odr)
        || applied[2] != (uint8_t)((config->accelerometer_range << 5) | odr)
        || (interfaces[0] & 0x70u) != 0x30u) {
        /* Require big-endian data and invalid markers before the first sample. */
        result = DART_IIM42652_CONFIGURATION_MISMATCH;
        goto finished;
    }
    diagnostic->stage = DART_IIM42652_STAGE_WAIT_DATA;
    diagnostic->first_read_ns = monotonic_time_ns();
    if (diagnostic->first_read_ns == 0u)
        goto clock_error;
    deadline = diagnostic->first_read_ns
        + (uint64_t)(config->ready_timeout_ms == 0u ? 1000u : config->ready_timeout_ms) * 1000000u;
    for (;;) {
        uint8_t bytes[14];
        if (read_status(sensor, diagnostic) != 0
            || read_registers(sensor, IIM42652_REG_TEMP_DATA1, bytes, sizeof(bytes)) != 0)
            goto finished;
        ++diagnostic->readiness_polls;
        decode_last_axes(diagnostic, bytes);
        if (config->skip_device_id_check)
            save_raw_probe(diagnostic, bytes);
        const uint64_t now = monotonic_time_ns();
        if (now == 0u)
            goto clock_error;
        if (plausible_raw_frame(bytes) && valid_axes(diagnostic->last_accelerometer)
            && valid_axes(diagnostic->last_gyroscope)) {
            diagnostic->ready_ns = now;
            diagnostic->stage = DART_IIM42652_STAGE_READY;
            result = DART_IIM42652_OK;
            break;
        }
        if (now >= deadline) {
            result = DART_IIM42652_READY_TIMEOUT;
            break;
        }
        if (delay_ms(5) != 0)
            goto clock_error;
    }
    /* Check independent byte reads on success or timeout; never mask the result. */
    diagnostic->individual_gyroscope_read = 1;
    for (unsigned int i = 0; i < 3u; ++i) {
        uint8_t high, low;
        if (read_registers(sensor, (uint8_t)(0x25u + i * 2u), &high, 1u) != 0
            || read_registers(sensor, (uint8_t)(0x26u + i * 2u), &low, 1u) != 0) {
            diagnostic->individual_gyroscope_read = 0;
            break;
        }
        diagnostic->individual_gyroscope[i] = signed_word(high, low);
    }
    goto finished;
clock_error:
    result = DART_IIM42652_CLOCK_ERROR;
finished:
    diagnostic->finished_ns = monotonic_time_ns();
    if (result == DART_IIM42652_OK && diagnostic->finished_ns == 0u)
        result = DART_IIM42652_CLOCK_ERROR;
    diagnostic->result = result;
    if (result != DART_IIM42652_OK) {
        dart_iim42652_deinit(&sensor);
        return result;
    }
    sensor->accelerometer_lsb_per_g = accelerometer_sensitivity(config->accelerometer_range);
    sensor->gyroscope_lsb_per_dps = gyroscope_sensitivity(config->gyroscope_range);
    *out_sensor = sensor;
    return DART_IIM42652_OK;
}

dart_iim42652_status_t
    dart_iim42652_read_sample(dart_iim42652_t* sensor, dart_iim42652_sample_t* sample) {
    uint8_t bytes[14];
    int16_t values[7];
    uint64_t timestamp;
    unsigned int index;
    if (sensor == NULL || sample == NULL || sensor->spi == NULL)
        return DART_IIM42652_NOT_INITIALIZED;
    if (read_registers(sensor, IIM42652_REG_TEMP_DATA1, bytes, sizeof(bytes)) != 0)
        return DART_IIM42652_BUS_ERROR;
    if (sensor->config.skip_device_id_check && !plausible_raw_frame(bytes))
        return DART_IIM42652_BUS_ERROR;
    timestamp = monotonic_time_ns();
    if (timestamp == 0u)
        return DART_IIM42652_CLOCK_ERROR;
    for (index = 0; index < 7; ++index)
        values[index] = signed_word(bytes[index * 2u], bytes[index * 2u + 1u]);
    memset(sample, 0, sizeof(*sample));
    sample->temperature_c = (float)values[0] / 132.48f + 25.0f;
    sample->accelerometer[0] = values[1];
    sample->accelerometer[1] = values[2];
    sample->accelerometer[2] = values[3];
    sample->gyroscope[0] = values[4];
    sample->gyroscope[1] = values[5];
    sample->gyroscope[2] = values[6];
    for (index = 0; index < 3; ++index) {
        sample->acceleration_m_s2[index] = (float)sample->accelerometer[index]
                                         * IIM42652_GRAVITY_M_S2 / sensor->accelerometer_lsb_per_g;
        sample->angular_velocity_rad_s[index] =
            (float)sample->gyroscope[index] * IIM42652_DEG_TO_RAD / sensor->gyroscope_lsb_per_dps
            - sensor->config.gyroscope_bias_rad_s[index];
    }
    sample->timestamp_ns = timestamp;
    if (sample->accelerometer[0] == INT16_MIN || sample->accelerometer[1] == INT16_MIN
        || sample->accelerometer[2] == INT16_MIN || sample->accelerometer[0] == INT16_MAX
        || sample->accelerometer[1] == INT16_MAX || sample->accelerometer[2] == INT16_MAX)
        sample->flags |= DART_IIM42652_SAMPLE_ACCEL_SATURATED;
    if (sample->gyroscope[0] == INT16_MIN || sample->gyroscope[1] == INT16_MIN
        || sample->gyroscope[2] == INT16_MIN || sample->gyroscope[0] == INT16_MAX
        || sample->gyroscope[1] == INT16_MAX || sample->gyroscope[2] == INT16_MAX)
        sample->flags |= DART_IIM42652_SAMPLE_GYRO_SATURATED;
    return DART_IIM42652_OK;
}

void dart_iim42652_deinit(dart_iim42652_t** sensor) {
    if (sensor == NULL || *sensor == NULL)
        return;
    if ((*sensor)->spi != NULL)
        drv_spi_inst_destroy(&(*sensor)->spi);
    free(*sensor);
    *sensor = NULL;
}
