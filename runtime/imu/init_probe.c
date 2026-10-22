#define _POSIX_C_SOURCE 200809L
/* Run with every other IMU/SPI user stopped. Does not start the camera. */
#include <inttypes.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "spi/iim42652.h"

static void notify_launcher(void) {
    /* RT-Smart waitpid/kill may retain a completed process. Use the same
     * explicit completion protocol as dart_app for finite foreground jobs. */
    const char* path = getenv("DART_EXIT_MARKER");
    if (path == NULL || path[0] != '/' || strlen(path) >= 512u) return;
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        char text[32];
        const int length = snprintf(text, sizeof(text), "%ld\n", (long)getpid());
        if (length > 0 && (size_t)length < sizeof(text)) {
            (void)write(fd, text, (size_t)length);
            (void)fsync(fd);
        }
        close(fd);
    }
}

int main(int argc, char** argv) {
    if (atexit(notify_launcher) != 0) return 2;
    const char* path = argc > 1 ? argv[1] : "/sdcard/app/logs/imu_init_probe.log";
    FILE* out = fopen(path, "w");
    if (out == NULL) { perror(path); return 2; }
    const struct { const char* name; uint32_t pre, post; } cases[] = {
        {"baseline", 0, 50}, {"pre200", 200, 50},
        {"post200", 0, 200}, {"both200", 200, 200}, {"post1000", 0, 1000}};
    fprintf(out, "probe=IIM-42652 version=1 ready_timeout_ms=1000 poll_ms=5\n");
    fflush(out);
    for (unsigned int run = 0; run < 2; ++run) {
        for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            dart_iim42652_config_t c = {0};
            c.bus = 1; c.chip_select_pin = 14; c.bus_hz = 1000000; c.timeout_ms = 20;
            c.pre_access_delay_ms = cases[i].pre;
            c.post_enable_delay_ms = cases[i].post;
            c.ready_timeout_ms = 1000;
            dart_iim42652_t* sensor = NULL;
            dart_iim42652_diagnostic_t d;
            const int rc = dart_iim42652_init_with_diagnostic(&c, &sensor, &d);
            fprintf(out,
                "run=%u case=%s pre_ms=%u post_ms=%u result=%d stage=%s "
                "id_read=%u id=0x%02x start_ns=%" PRIu64 " first_access_ns=%" PRIu64
                " reset_ns=%" PRIu64 " configured_ns=%" PRIu64 " enabled_ns=%" PRIu64
                " first_read_ns=%" PRIu64 " ready_ns=%" PRIu64 " finished_ns=%" PRIu64
                " polls=%u config_read=%u intf0=0x%02x intf1=0x%02x power=0x%02x "
                "gyro_cfg=0x%02x accel_cfg=0x%02x status=0x%02x status_seen=0x%02x "
                "axis_config_read=%u sensor_config0=0x%02x accel=%d,%d,%d gyro=%d,%d,%d "
                "individual_read=%u gyro_individual=%d,%d,%d\n",
                run, cases[i].name, c.pre_access_delay_ms, c.post_enable_delay_ms,
                rc, dart_iim42652_stage_name(d.stage), d.device_id_read, d.observed_device_id,
                d.started_ns, d.first_access_ns, d.reset_ns, d.configured_ns, d.enabled_ns,
                d.first_read_ns, d.ready_ns, d.finished_ns, d.readiness_polls, d.registers_read,
                d.interface_config0, d.interface_config1, d.power_config, d.gyro_config,
                d.accel_config, d.interrupt_status, d.interrupt_status_seen,
                d.sensor_config_read, d.sensor_config0, d.last_accelerometer[0],
                d.last_accelerometer[1], d.last_accelerometer[2], d.last_gyroscope[0],
                d.last_gyroscope[1], d.last_gyroscope[2], d.individual_gyroscope_read,
                d.individual_gyroscope[0], d.individual_gyroscope[1], d.individual_gyroscope[2]);
            fflush(out);
            printf("imu_probe run=%u case=%s result=%d polls=%u\n", run, cases[i].name,
                rc, d.readiness_polls);
            fflush(stdout);
            dart_iim42652_deinit(&sensor);
        }
    }
    fprintf(out, "probe=complete\n");
    const int failed = ferror(out);
    if (fclose(out) != 0 || failed) return 3;
    return 0; /* Successful diagnostic execution; per-case results are in the log. */
}
