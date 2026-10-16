

#include <cassert>

#include "app/argument_parser.cpp"

namespace dart::tests {

class ArgumentParserTest final {
public:
    static void run() {
        char program[] = "dart_app";
        char* defaults[] = {program};
        auto parsed = app::parse_args(1, defaults);
        assert(parsed && parsed->sensor_width == 1920 && parsed->record_on);
        assert(!parsed->imu_on);
        char record[] = "--record";
        char off[] = "off";
        char seconds[] = "--seconds";
        char ten[] = "10";
        char* valid[] = {program, record, off, seconds, ten};
        parsed = app::parse_args(5, valid);
        assert(parsed && !parsed->record_on && parsed->seconds == 10);
        char unknown[] = "--unknown";
        char* invalid[] = {program, unknown};
        assert(!app::parse_args(2, invalid));
        char* missing[] = {program, seconds};
        assert(!app::parse_args(2, missing));
        char imu[] = "--imu";
        char on[] = "on";
        char bus[] = "--imu-bus";
        char cs[] = "--imu-cs";
        char zero[] = "0";
        char hal_cs[] = "-1";
        char too_large[] = "64";
        char imu_log[] = "--imu-log";
        char path[] = "/tmp/imu.log";
        char* no_bus[] = {program, imu, on};
        assert(!app::parse_args(3, no_bus));
        char* imu_valid[] = {program, imu, on, bus, zero, cs, hal_cs, imu_log, path};
        parsed = app::parse_args(9, imu_valid);
        assert(parsed && parsed->imu_on && parsed->imu_bus == 0 && parsed->imu_cs == -1);
        assert(std::string_view{parsed->imu_log} == path);
        char* imu_invalid[] = {program, cs, too_large};
        assert(!app::parse_args(3, imu_invalid));
        char empty[] = "";
        char* empty_log[] = {program, imu_log, empty};
        assert(!app::parse_args(3, empty_log));
    }
};

} // namespace dart::tests

int main() { dart::tests::ArgumentParserTest::run(); }
