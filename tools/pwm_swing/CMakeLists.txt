# Optional board diagnostic, built explicitly with --target pwm_swing.
add_executable(pwm_swing main.c)
target_compile_options(pwm_swing PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(pwm_swing PRIVATE k230_rtos_app m)
set_target_properties(pwm_swing PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pwm_swing")
