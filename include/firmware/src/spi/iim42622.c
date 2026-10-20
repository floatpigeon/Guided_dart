#define _POSIX_C_SOURCE 200809L

#include "spi/iim42622.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "drv_spi.h"

#define IIM42622_REG_DEVICE_CONFIG        0x11u
#define IIM42622_REG_TEMP_DATA1           0x1du
#define IIM42622_REG_WHO_AM_I             0x75u
#define IIM42622_REG_PWR_MGMT0            0x4eu
#define IIM42622_REG_GYRO_CONFIG0         0x4fu
#define IIM42622_REG_ACCEL_CONFIG0        0x50u
#define IIM42622_SOFT_RESET               0x01u
#define IIM42622_PWR_GYRO_ACCEL_LOW_NOISE 0x0fu
#define IIM42622_DEFAULT_ODR              0x06u
#define IIM42622_GRAVITY_M_S2             9.80665f
#define IIM42622_DEG_TO_RAD               0.01745329251994329577f

struct dart_iim42622 {
    dart_iim42622_config_t config;
    drv_spi_inst_t spi;
    float accelerometer_lsb_per_g;
    float gyroscope_lsb_per_dps;
};

const char* dart_iim42622_device_name(uint8_t device_id) {
    switch (device_id) {
    case DART_IMU_DEVICE_ID_ICM42622P: return "ICM-42622-P";
    case DART_IMU_DEVICE_ID_IIM42652: return "IIM-42652";
    default: return "unknown";
    }
}

static int supported_device_id(uint8_t device_id) {
    return device_id == DART_IMU_DEVICE_ID_ICM42622P || device_id == DART_IMU_DEVICE_ID_IIM42652;
}

static int delay_ms(long milliseconds) {
    struct timespec remaining = {
        .tv_sec = milliseconds / 1000, .tv_nsec = (milliseconds % 1000) * 1000000L};
    while (nanosleep(&remaining, &remaining) != 0)
        if (errno != EINTR)
            return -1;
    return 0;
}

static int valid_config(const dart_iim42622_config_t* config) {
    unsigned int index;
    if (config == NULL || config->bus < 0 || config->bus > 2 || config->bus_hz == 0u
        || config->timeout_ms == 0u || config->chip_select_pin < -1 || config->chip_select_pin > 63
        || config->accelerometer_range > DART_IIM42622_ACCEL_2G
        || config->gyroscope_range > DART_IIM42622_GYRO_15_625DPS
        || config->output_data_rate > 0x0fu)
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

static int write_register(dart_iim42622_t* sensor, uint8_t reg, uint8_t value) {
    const uint8_t bytes[2] = {(uint8_t)(reg & 0x7fu), value};
    if (sensor == NULL || sensor->spi == NULL)
        return -1;
    return drv_spi_write(sensor->spi, bytes, sizeof(bytes), true) == 2 ? 0 : -1;
}

static int read_registers(dart_iim42622_t* sensor, uint8_t reg, uint8_t* buffer, uint16_t length) {
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

dart_iim42622_status_t
    dart_iim42622_init(const dart_iim42622_config_t* config, dart_iim42622_t** out_sensor) {
    return dart_iim42622_init_with_diagnostic(config, out_sensor, NULL);
}

dart_iim42622_status_t dart_iim42622_init_with_diagnostic(
    const dart_iim42622_config_t* config, dart_iim42622_t** out_sensor,
    dart_iim42622_diagnostic_t* diagnostic) {
    dart_iim42622_t* sensor;
    uint8_t device_id = 0;
    uint8_t odr;
    if (diagnostic != NULL) {
        memset(diagnostic, 0, sizeof(*diagnostic));
        diagnostic->expected_device_id = DART_IMU_DEVICE_ID_ICM42622P;
    }
    if (out_sensor == NULL)
        return DART_IIM42622_INVALID_ARGUMENT;
    if (*out_sensor != NULL)
        return DART_IIM42622_ALREADY_INITIALIZED;
    if (!valid_config(config))
        return DART_IIM42622_INVALID_ARGUMENT;
    sensor = (dart_iim42622_t*)calloc(1u, sizeof(*sensor));
    if (sensor == NULL)
        return DART_IIM42622_NO_MEMORY;
    sensor->config = *config;
    if (drv_spi_inst_create(
            config->bus, true, SPI_HAL_MODE_3, config->bus_hz, 8u, config->chip_select_pin,
            SPI_HAL_DATA_LINE_1, &sensor->spi)
        != 0) {
        /* The SDK factory frees its partial instance on failure but leaves
         * a dangling output pointer. Do not destroy it a second time. */
        free(sensor);
        return DART_IIM42622_BUS_ERROR;
    }
    if (read_registers(sensor, IIM42622_REG_WHO_AM_I, &device_id, 1u) != 0) {
        dart_iim42622_deinit(&sensor);
        return DART_IIM42622_BUS_ERROR;
    }
    if (diagnostic != NULL) {
        diagnostic->device_id_read = 1;
        diagnostic->observed_device_id = device_id;
    }
    if (!supported_device_id(device_id)) {
        dart_iim42622_deinit(&sensor);
        return DART_IIM42622_UNEXPECTED_DEVICE_ID;
    }
    if (diagnostic != NULL)
        diagnostic->expected_device_id = device_id;
    if (write_register(sensor, IIM42622_REG_DEVICE_CONFIG, IIM42622_SOFT_RESET) != 0) {
        dart_iim42622_deinit(&sensor);
        return DART_IIM42622_BUS_ERROR;
    }
    if (delay_ms(2) != 0) {
        dart_iim42622_deinit(&sensor);
        return DART_IIM42622_CLOCK_ERROR;
    }
    uint8_t reset_id = 0;
    if (read_registers(sensor, IIM42622_REG_WHO_AM_I, &reset_id, 1u) != 0
        || reset_id != device_id) {
        dart_iim42622_deinit(&sensor);
        return DART_IIM42622_BUS_ERROR;
    }
    odr = config->output_data_rate == 0u ? IIM42622_DEFAULT_ODR : config->output_data_rate;
    /* IIM-42652 DS-000440: configure while asleep, then power on.
     * No register writes for 200 us after enabling; gyro startup is 45 ms. */
    if (write_register(
            sensor, IIM42622_REG_GYRO_CONFIG0, (uint8_t)((config->gyroscope_range << 5) | odr))
            != 0
        || write_register(
               sensor, IIM42622_REG_ACCEL_CONFIG0,
               (uint8_t)((config->accelerometer_range << 5) | odr))
               != 0
        || write_register(sensor, IIM42622_REG_PWR_MGMT0, IIM42622_PWR_GYRO_ACCEL_LOW_NOISE) != 0) {
        dart_iim42622_deinit(&sensor);
        return DART_IIM42622_BUS_ERROR;
    }
    if (delay_ms(50) != 0) {
        dart_iim42622_deinit(&sensor);
        return DART_IIM42622_CLOCK_ERROR;
    }
    uint8_t applied[3];
    if (read_registers(sensor, IIM42622_REG_PWR_MGMT0, applied, 3u) != 0) {
        dart_iim42622_deinit(&sensor);
        return DART_IIM42622_BUS_ERROR;
    }
    if (applied[0] != IIM42622_PWR_GYRO_ACCEL_LOW_NOISE
        || applied[1] != (uint8_t)((config->gyroscope_range << 5) | odr)
        || applied[2] != (uint8_t)((config->accelerometer_range << 5) | odr)) {
        dart_iim42622_deinit(&sensor);
        return DART_IIM42622_CONFIGURATION_MISMATCH;
    }
    if (diagnostic != NULL) {
        uint8_t interfaces[2], status, gyro[6];
        int diagnostic_ok = read_registers(sensor, 0x4cu, interfaces, 2u) == 0
                         && read_registers(sensor, 0x2du, &status, 1u) == 0;
        for (unsigned int i = 0; diagnostic_ok && i < sizeof(gyro); ++i)
            diagnostic_ok = read_registers(sensor, (uint8_t)(0x25u + i), &gyro[i], 1u) == 0;
        if (diagnostic_ok) {
            diagnostic->registers_read = 1;
            diagnostic->interface_config0 = interfaces[0];
            diagnostic->interface_config1 = interfaces[1];
            diagnostic->power_config = applied[0];
            diagnostic->gyro_config = applied[1];
            diagnostic->accel_config = applied[2];
            diagnostic->interrupt_status = status;
            for (unsigned int i = 0; i < 3; ++i)
                diagnostic->individual_gyroscope[i] = signed_word(gyro[i * 2], gyro[i * 2 + 1]);
        }
    }
    sensor->accelerometer_lsb_per_g = accelerometer_sensitivity(config->accelerometer_range);
    sensor->gyroscope_lsb_per_dps = gyroscope_sensitivity(config->gyroscope_range);
    *out_sensor = sensor;
    return DART_IIM42622_OK;
}

dart_iim42622_status_t
    dart_iim42622_read_sample(dart_iim42622_t* sensor, dart_iim42622_sample_t* sample) {
    uint8_t bytes[14];
    int16_t values[7];
    uint64_t timestamp;
    unsigned int index;
    if (sensor == NULL || sample == NULL || sensor->spi == NULL)
        return DART_IIM42622_NOT_INITIALIZED;
    if (read_registers(sensor, IIM42622_REG_TEMP_DATA1, bytes, sizeof(bytes)) != 0)
        return DART_IIM42622_BUS_ERROR;
    timestamp = monotonic_time_ns();
    if (timestamp == 0u)
        return DART_IIM42622_CLOCK_ERROR;
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
                                         * IIM42622_GRAVITY_M_S2 / sensor->accelerometer_lsb_per_g;
        sample->angular_velocity_rad_s[index] =
            (float)sample->gyroscope[index] * IIM42622_DEG_TO_RAD / sensor->gyroscope_lsb_per_dps
            - sensor->config.gyroscope_bias_rad_s[index];
    }
    sample->timestamp_ns = timestamp;
    if (sample->accelerometer[0] == INT16_MIN || sample->accelerometer[1] == INT16_MIN
        || sample->accelerometer[2] == INT16_MIN || sample->accelerometer[0] == INT16_MAX
        || sample->accelerometer[1] == INT16_MAX || sample->accelerometer[2] == INT16_MAX)
        sample->flags |= DART_IIM42622_SAMPLE_ACCEL_SATURATED;
    if (sample->gyroscope[0] == INT16_MIN || sample->gyroscope[1] == INT16_MIN
        || sample->gyroscope[2] == INT16_MIN || sample->gyroscope[0] == INT16_MAX
        || sample->gyroscope[1] == INT16_MAX || sample->gyroscope[2] == INT16_MAX)
        sample->flags |= DART_IIM42622_SAMPLE_GYRO_SATURATED;
    return DART_IIM42622_OK;
}

void dart_iim42622_deinit(dart_iim42622_t** sensor) {
    if (sensor == NULL || *sensor == NULL)
        return;
    if ((*sensor)->spi != NULL)
        drv_spi_inst_destroy(&(*sensor)->spi);
    free(*sensor);
    *sensor = NULL;
}
