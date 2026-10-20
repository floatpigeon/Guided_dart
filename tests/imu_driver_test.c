#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "drv_spi.h"
#include "spi/iim42622.h"

static int create_failed, fail_read_at, fail_write_at, live, destroys;
static int reads, writes, corrupt_config, change_reset_id;
static uint8_t device_id, registers[128];
static struct timespec powered_at;
static int instance;

int drv_spi_inst_create(
    int bus, bool active_low, int mode, uint32_t baudrate, uint8_t bits, int cs, uint8_t lines,
    drv_spi_inst_t* out) {
    assert(bus == 1 && active_low && mode == SPI_HAL_MODE_3 && baudrate == 1000000);
    assert(bits == 8 && cs == 14 && lines == SPI_HAL_DATA_LINE_1);
    /* Match the SDK factory's failure contract: non-null output, no live handle. */
    *out = (drv_spi_inst_t)&instance;
    if (create_failed)
        return -1;
    live = 1;
    return 0;
}

void drv_spi_inst_destroy(drv_spi_inst_t* handle) {
    assert(live && *handle == (drv_spi_inst_t)&instance);
    ++destroys;
    live = 0;
}

int drv_spi_transfer(
    drv_spi_inst_t handle, const void* tx, void* rx, size_t length, bool cs_change) {
    assert(live && handle == (drv_spi_inst_t)&instance && cs_change);
    if (++reads == fail_read_at)
        return -1;
    const uint8_t command = ((const uint8_t*)tx)[0];
    memset(rx, 0, length);
    if (command == 0xf5) {
        assert(length == 2);
        ((uint8_t*)rx)[1] = change_reset_id && reads > 1 ? 0xff : device_id;
    } else if (command == 0xce) {
        assert(length == 4);
        memcpy((uint8_t*)rx + 1, registers + 0x4e, 3);
        if (corrupt_config)
            ((uint8_t*)rx)[1] = 0;
    } else if (command == 0xcc) {
        assert(length == 3);
        ((uint8_t*)rx)[1] = 0x30;
        ((uint8_t*)rx)[2] = 0x91;
    } else if (command == 0xad) {
        assert(length == 2);
        ((uint8_t*)rx)[1] = 0x18;
    } else if (command >= 0xa5 && command <= 0xaa) {
        const uint8_t values[] = {0x01, 0x0d, 0x00, 0x00, 0xfd, 0xe6};
        assert(length == 2);
        ((uint8_t*)rx)[1] = values[command - 0xa5];
    } else {
        assert(command == 0x9d && length == 15);
        assert(writes == 4 && registers[0x4e] == 0x0f);
        struct timespec now;
        assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        const int64_t elapsed = (now.tv_sec - powered_at.tv_sec) * INT64_C(1000000000) + now.tv_nsec
                              - powered_at.tv_nsec;
        assert(elapsed >= 45000000); /* No samples during gyro startup. */
        /* 26 C, +1 g on X, -0.5 g on Y, +16.4 dps on X, -32.8 dps on Z. */
        const uint8_t sample[] = {0x00, 0x84, 0x08, 0x00, 0xfc, 0x00, 0x00,
                                  0x00, 0x01, 0x0d, 0x00, 0x00, 0xfd, 0xe6};
        memcpy((uint8_t*)rx + 1, sample, sizeof(sample));
    }
    return (int)length;
}

int drv_spi_write(drv_spi_inst_t handle, const void* tx, size_t length, bool change) {
    assert(live && handle == (drv_spi_inst_t)&instance && change && length == 2);
    if (++writes == fail_write_at)
        return -1;
    const uint8_t* bytes = tx;
    /* Model the device rule: configure before enabling, no writes after enable. */
    assert(registers[0x4e] == 0);
    if (bytes[0] == 0x11) {
        assert(bytes[1] == 1 && writes == 1);
        memset(registers, 0, sizeof(registers));
    } else if (bytes[0] == 0x4f || bytes[0] == 0x50) {
        registers[bytes[0]] = bytes[1];
    } else {
        assert(bytes[0] == 0x4e && bytes[1] == 0x0f && writes == 4);
        assert(registers[0x4f] == 6 && registers[0x50] == 6);
        registers[0x4e] = bytes[1];
        assert(clock_gettime(CLOCK_MONOTONIC, &powered_at) == 0);
    }
    return (int)length;
}

static void clear_fake(uint8_t id) {
    assert(!live);
    create_failed = fail_read_at = fail_write_at = destroys = reads = writes = 0;
    corrupt_config = change_reset_id = 0;
    device_id = id;
    memset(registers, 0, sizeof(registers));
}

int main(void) {
    dart_iim42622_config_t config = {0};
    config.bus = 1;
    config.chip_select_pin = 14;
    config.bus_hz = 1000000;
    config.timeout_ms = 20;
    for (int test = 0; test < 16; ++test) {
        clear_fake(test >= 4 ? 0x6f : 0x46);
        create_failed = test == 0;
        if (test == 1)
            fail_read_at = 1;
        if (test == 2)
            device_id = 0xff;
        if (test == 5)
            device_id = 0x00;
        if (test == 6)
            device_id = 0x47; /* Do not silently accept other variants. */
        if (test == 7)
            change_reset_id = 1;
        if (test >= 8 && test <= 11)
            fail_write_at = test - 7;
        if (test == 12)
            fail_read_at = 2;
        if (test == 13)
            fail_read_at = 3;
        if (test == 14)
            corrupt_config = 1;
        dart_iim42622_t* sensor = NULL;
        dart_iim42622_diagnostic_t diagnostic;
        const int rc = dart_iim42622_init_with_diagnostic(&config, &sensor, &diagnostic);
        assert(diagnostic.device_id_read == (test >= 2));
        if (test >= 2)
            assert(diagnostic.observed_device_id == device_id);
        if (test == 3 || test == 4 || test == 15) {
            assert(rc == DART_IIM42622_OK && sensor != NULL && live);
            assert(diagnostic.expected_device_id == device_id);
            assert(
                diagnostic.registers_read && diagnostic.interface_config0 == 0x30
                && diagnostic.interface_config1 == 0x91 && diagnostic.power_config == 0x0f);
            assert(
                diagnostic.gyro_config == 6 && diagnostic.accel_config == 6
                && diagnostic.interrupt_status == 0x18);
            assert(
                diagnostic.individual_gyroscope[0] == 269
                && diagnostic.individual_gyroscope[2] == -538);
            assert(
                strcmp(
                    dart_iim42622_device_name(device_id),
                    device_id == 0x46 ? "ICM-42622-P" : "IIM-42652")
                == 0);
            dart_iim42622_sample_t sample;
            assert(dart_iim42622_read_sample(sensor, &sample) == DART_IIM42622_OK);
            assert(sample.timestamp_ns > 0 && sample.flags == 0);
            assert(sample.accelerometer[0] == 2048 && sample.accelerometer[1] == -1024);
            assert(fabsf(sample.acceleration_m_s2[0] - 9.80665f) < 0.0001f);
            assert(fabsf(sample.angular_velocity_rad_s[2] + 0.57253f) < 0.001f);
            assert(fabsf(sample.temperature_c - 26.0f) < 0.01f);
            dart_iim42622_deinit(&sensor);
            dart_iim42622_deinit(&sensor);
            assert(sensor == NULL && !live && destroys == 1);
        } else {
            const int expected = test == 2 || test == 5 || test == 6
                                   ? DART_IIM42622_UNEXPECTED_DEVICE_ID
                               : test == 14 ? DART_IIM42622_CONFIGURATION_MISMATCH
                                            : DART_IIM42622_BUS_ERROR;
            assert(rc == expected);
            assert(sensor == NULL && !live && destroys == (test != 0));
        }
    }
    assert(strcmp(dart_iim42622_device_name(0), "unknown") == 0);
    clear_fake(0x6f);
    dart_iim42622_t* sensor = NULL;
    assert(dart_iim42622_init(&config, &sensor) == DART_IIM42622_OK);
    dart_iim42622_deinit(&sensor);
    return 0;
}
