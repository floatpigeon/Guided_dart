#ifndef DART_EXPOSURE_PLAN_H
#define DART_EXPOSURE_PLAN_H
#include "camera.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Pure calculation: hardware range in us, integration command in seconds. */
dart_status_t dart_exposure_plan(uint32_t requested_us, float min_us, float max_us,
                                 dart_exposure_result_t *result, float *seconds);
#ifdef __cplusplus
}
#endif
#endif
