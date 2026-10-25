#ifndef DART_TESTS_ODR_CONSUMER_CPP
#define DART_TESTS_ODR_CONSUMER_CPP

#include "camera/frame_lease.cpp"
#include "imu/estimator.cpp"
#include "pwm/session.cpp"
#include "recorder/recorder.cpp"
#include "src/control/servo_mapping.cpp"
#include "src/detector/roi.cpp"

namespace dart::tests {

class OdrConsumer final {
public:
    [[gnu::used]] static bool check() noexcept;
};

// Deliberately emit references from another translation unit, exercising the
// inline definitions shared with the main test executable at link time.
inline bool OdrConsumer::check() noexcept {
    imu::Estimator estimator;
    detection::RoiDetector detector;
    recorder::Recorder recording{recorder::Config{}};
    control::ServoMapping mapping;
    return !estimator.current().has_reference_frame && detector.Prepare(64, 64)
        && !recording.started() && mapping.pulses().has_value()
        && pwm::valid_pulses(mapping.pulses()->pulse_us);
}

} // namespace dart::tests

#endif // DART_TESTS_ODR_CONSUMER_CPP
