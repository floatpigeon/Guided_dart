#include "exposure_plan.h"
#include <math.h>
#include <string.h>

dart_status_t dart_exposure_plan(uint32_t requested_us, float min_us, float max_us,
                                 dart_exposure_result_t *result, float *seconds) {
    if (result == NULL || seconds == NULL) return DART_STATUS_INVALID_ARGUMENT;
    memset(result, 0, sizeof(*result));
    *seconds = 0.0f;
    if (requested_us == 0) return DART_STATUS_INVALID_ARGUMENT;
    if (!isfinite(min_us) || !isfinite(max_us) || min_us <= 0.0f ||
        max_us < min_us || (double)max_us > UINT32_MAX) {
        return DART_STATUS_HARDWARE_ERROR;
    }
    /* Round inward: 27.375..999.75 us permits integer commands 28..999.
     * Rounding to nearest can silently send commands outside the legal range.
     */
    const double lower = ceil((double)min_us);
    const double upper = floor((double)max_us);
    if (lower > upper) return DART_STATUS_OUT_OF_RANGE;
    uint32_t applied = requested_us;
    if ((double)applied < lower) applied = (uint32_t)lower;
    if ((double)applied > upper) applied = (uint32_t)upper;
    *result = (dart_exposure_result_t){DART_CAMERA_ABI_VERSION, sizeof(*result),
        requested_us, applied, min_us, max_us, applied != requested_us};
    *seconds = (float)((double)applied / 1000000.0);
    return DART_STATUS_OK;
}
