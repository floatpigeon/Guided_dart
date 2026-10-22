

#include <cassert>

#include "src/startup/argument_parser.cpp"
#include "src/recording/frame_recording.cpp"

namespace dart::tests {

class ArgumentParserTest final {
public:
    static void run() {
        char program[] = "dart_app";
        char* defaults[] = {program};
        auto parsed = app::parse_args(1, defaults);
        assert(parsed && parsed->record_on && parsed->exposure_lines == 2560);
        assert(parsed->record_fps == 60 && parsed->record_keep == 0);
        assert(parsed->record_format == recorder::ImageFormat::kPng && parsed->threshold == 50);
        assert(app::recording_config(*parsed, 640, 480).max_storage_bytes == 2'000'000'000);
        for (const auto directory : {"/data", "/data/", "/data/custom", "/database", "/tmp/records"}) {
            app::Arguments args;
            std::snprintf(args.rec_dir, sizeof(args.rec_dir), "%s", directory);
            const bool data_volume = std::string_view{directory}.starts_with("/data")
                                  && std::string_view{directory} != "/database";
            assert(app::recording_config(args, 640, 480).max_storage_bytes
                   == (data_volume ? 2'000'000'000 : 0));
        }
        char format[] = "--rec-format";
        char png[] = "png", pgm[] = "pgm", pbm[] = "pbm", bad_format[] = "jpg";
        for (char* value : {png, pgm, pbm}) {
            char* options[] = {program, format, value};
            const auto chosen = app::parse_args(3, options);
            assert(chosen && chosen->record_format == (value == png ? recorder::ImageFormat::kPng
                : value == pgm ? recorder::ImageFormat::kPgm : recorder::ImageFormat::kPbm));
        }
        char* bad_options[] = {program, format, bad_format};
        assert(!app::parse_args(3, bad_options));
        assert(parsed->raw10_layout == camera::Raw10Layout::kPackedLsb);
        assert(!parsed->imu_on && !parsed->imu_ignore_id);
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
        char capture_fps[] = "--fps";
        char capture_size[] = "--acq";
        char output_size[] = "--out";
        char csi[] = "--csi";
        char mirror[] = "--mirror";
        for (char* option : {capture_fps, capture_size, output_size, csi, mirror}) {
            char* old_option[] = {program, option, ten};
            assert(!app::parse_args(3, old_option));
        }
        char rec_fps[] = "--rec-fps";
        char* rate[] = {program, rec_fps, ten};
        parsed = app::parse_args(3, rate);
        assert(parsed && parsed->record_fps == 10);
        char ae[] = "--ae";
        char* automatic_exposure[] = {program, ae};
        parsed = app::parse_args(2, automatic_exposure);
        assert(!parsed);
        char exp[] = "--exp";
        char exposure[] = "2560";
        char preserve[] = "0";
        char* fixed_exposure[] = {program, exp, exposure};
        parsed = app::parse_args(3, fixed_exposure);
        assert(parsed && parsed->exposure_lines == 2560);
        fixed_exposure[2] = preserve;
        parsed = app::parse_args(3, fixed_exposure);
        assert(parsed && parsed->exposure_lines == 0);
        char* conflicting_exposure[] = {program, ae, exp, ten};
        assert(!app::parse_args(4, conflicting_exposure));
        char raw_layout[] = "--raw-layout";
        char msb[] = "le16-msb";
        char packed[] = "mipi10";
        char* raw_args[] = {program, raw_layout, msb};
        parsed = app::parse_args(3, raw_args);
        assert(parsed && parsed->raw10_layout == camera::Raw10Layout::kLe16Msb);
        raw_args[2] = packed;
        parsed = app::parse_args(3, raw_args);
        assert(parsed && parsed->raw10_layout == camera::Raw10Layout::kMipi);
        char mcm_packed[] = "packed10-lsb";
        raw_args[2] = mcm_packed;
        parsed = app::parse_args(3, raw_args);
        assert(parsed && parsed->raw10_layout == camera::Raw10Layout::kPackedLsb);
        raw_args[2] = ten;
        assert(!app::parse_args(3, raw_args));
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
        char ignore_id[] = "--imu-ignore-id";
        char* bypass[] = {program, imu, on, bus, zero, cs, hal_cs, ignore_id, on};
        parsed = app::parse_args(9, bypass);
        assert(parsed && parsed->imu_on && parsed->imu_ignore_id);
        bypass[8] = off;
        parsed = app::parse_args(9, bypass);
        assert(parsed && !parsed->imu_ignore_id);
        bypass[8] = ten;
        assert(!app::parse_args(9, bypass));
        assert(!app::parse_args(8, bypass));
    }
};

} // namespace dart::tests

int main() { dart::tests::ArgumentParserTest::run(); }
