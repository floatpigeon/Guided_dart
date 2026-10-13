#ifndef DART_FIRMWARE_IIM42622_H
#define DART_FIRMWARE_IIM42622_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DART_IIM42622_OK = 0,
    DART_IIM42622_INVALID_ARGUMENT = -1,
    DART_IIM42622_ALREADY_INITIALIZED = -2,
    DART_IIM42622_NOT_INITIALIZED = -3,
    DART_IIM42622_NO_MEMORY = -4,
    DART_IIM42622_BUS_ERROR = -5,
    DART_IIM42622_UNEXPECTED_DEVICE_ID = -6,
    DART_IIM42622_CLOCK_ERROR = -7,
} dart_iim42622_status_t;

typedef enum {
    DART_IIM42622_ACCEL_16G = 0,
    DART_IIM42622_ACCEL_8G = 1,
    DART_IIM42622_ACCEL_4G = 2,
    DART_IIM42622_ACCEL_2G = 3,
} dart_iim42622_accel_range_t;

typedef enum {
    DART_IIM42622_GYRO_2000DPS = 0,
    DART_IIM42622_GYRO_1000DPS = 1,
    DART_IIM42622_GYRO_500DPS = 2,
    DART_IIM42622_GYRO_250DPS = 3,
    DART_IIM42622_GYRO_125DPS = 4,
    DART_IIM42622_GYRO_62_5DPS = 5,
    DART_IIM42622_GYRO_31_25DPS = 6,
    DART_IIM42622_GYRO_15_625DPS = 7,
} dart_iim42622_gyro_range_t;

typedef struct {
    int bus;
    uint32_t bus_hz;
    uint32_t timeout_ms;
    int chip_select_pin;
    uint8_t accelerometer_range;
    uint8_t gyroscope_range;
    uint8_t output_data_rate;
    float gyroscope_bias_rad_s[3];
} dart_iim42622_config_t;

enum {
    DART_IIM42622_SAMPLE_ACCEL_SATURATED = 1u << 0,
    DART_IIM42622_SAMPLE_GYRO_SATURATED = 1u << 1,
};

typedef struct {
    int16_t accelerometer[3];
    int16_t gyroscope[3];
    float acceleration_m_s2[3];
    float angular_velocity_rad_s[3];
    float temperature_c;
    uint64_t timestamp_ns;
    uint32_t flags;
} dart_iim42622_sample_t;

typedef struct dart_iim42622 dart_iim42622_t;

dart_iim42622_status_t dart_iim42622_init(const dart_iim42622_config_t *config,
                                          dart_iim42622_t **out_sensor);
dart_iim42622_status_t dart_iim42622_read_sample(
    dart_iim42622_t *sensor, dart_iim42622_sample_t *sample);
void dart_iim42622_deinit(dart_iim42622_t **sensor);

#ifdef __cplusplus
}
#endif

#endif
