

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
    }
};

} // namespace dart::tests

int main() { dart::tests::ArgumentParserTest::run(); }
