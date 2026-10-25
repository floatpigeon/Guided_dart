/* SPDX-License-Identifier: MIT
 * MicroPython adapter for the application's unchanged RAW10 camera backend.
 * All calls run on the Python thread; no background camera or PWM task.
 */
#include <string.h>

#include "py/runtime.h"
#include "camera/c_api/camera.h"

typedef struct {
    mp_obj_base_t base;
    dart_media_t *media;
    dart_camera_t *camera;
    uint64_t sequence, received_us, source_pts;
    uint32_t flags, threshold;
    bool has_frame;
} raw_camera_t;

static int raw_close(raw_camera_t *self) {
    int rc;
    if (self->camera != NULL) {
        self->has_frame = false;
        uint32_t state;
        rc = dart_camera_get_state(self->camera, &state);
        if (rc != DART_CAMERA_OK)
            return rc;
        if (state != DART_CAMERA_STATE_EMPTY) {
            rc = dart_camera_deinit(self->camera);
            if (rc != DART_CAMERA_OK)
                return rc;
        }
        rc = dart_camera_destroy(self->camera);
        if (rc != DART_CAMERA_OK)
            return rc;
        self->camera = NULL;
    }
    if (self->media != NULL) {
        rc = dart_media_destroy(self->media);
        if (rc != DART_CAMERA_OK)
            return rc;
        self->media = NULL;
    }
    return DART_CAMERA_OK;
}

static void raw_check(int rc, const char *operation) {
    if (rc != DART_CAMERA_OK)
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("RAW camera %s failed: %d"),
                         operation, rc);
}

static mp_obj_t raw_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw,
                            const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 3, 3, false);
    const mp_int_t lines = mp_obj_get_int(args[0]);
    const mp_int_t threshold = mp_obj_get_int(args[1]);
    const mp_int_t mirror = mp_obj_get_int(args[2]);
    if ((lines != 0 && (lines < 4 || lines > 65509)) || threshold < 0 || threshold > 255
        || mirror < 0 || mirror > 3)
        mp_raise_ValueError(MP_ERROR_TEXT("invalid exposure lines, threshold, or mirror"));

    raw_camera_t *self = m_new_obj_with_finaliser(raw_camera_t);
    self->base.type = type;
    self->media = NULL;
    self->camera = NULL;
    self->has_frame = false;
    dart_camera_config_t config;
    dart_camera_config_init(&config);
    config.csi_port = 2;
    config.sensor_width = config.output_width = 640;
    config.sensor_height = config.output_height = 480;
    config.sensor_fps = 0;
    config.capture_buffers = 4;
    config.channel_buffers = 4;
    config.max_outstanding_frames = 1;
    config.exposure_mode = DART_CAMERA_EXPOSURE_FIXED;
    config.initial_exposure_lines = (uint32_t)lines;
    self->threshold = (uint32_t)threshold; // Statistics only; pixels remain grayscale.
    config.raw10_layout = DART_CAMERA_RAW10_PACKED_LSB;
    config.mirror = (uint32_t)mirror;

    int rc = dart_media_create(&self->media);
    if (rc == DART_CAMERA_OK)
        rc = dart_camera_create(&self->camera);
    if (rc == DART_CAMERA_OK)
        rc = dart_camera_init(self->camera, self->media, &config);
    if (rc == DART_CAMERA_OK)
        rc = dart_camera_start(self->camera);
    if (rc != DART_CAMERA_OK) {
        (void)raw_close(self);
        raw_check(rc, "open");
    }
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t raw_snapshot_into(mp_obj_t self_in, mp_obj_t buffer_in, mp_obj_t timeout_in) {
    raw_camera_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->camera == NULL)
        mp_raise_ValueError(MP_ERROR_TEXT("camera is closed"));
    const mp_int_t timeout = mp_obj_get_int(timeout_in);
    if (timeout < 0 || timeout > 1000)
        mp_raise_ValueError(MP_ERROR_TEXT("timeout must be 0..1000 ms"));
    mp_buffer_info_t output;
    mp_get_buffer_raise(buffer_in, &output, MP_BUFFER_WRITE);
    if (output.len != 640u * 480u)
        mp_raise_ValueError(MP_ERROR_TEXT("expected writable 640x480 Y8 buffer"));

    dart_camera_frame_t frame;
    self->has_frame = false;
    int rc = dart_camera_acquire_frame(self->camera, (uint32_t)timeout, &frame);
    if (rc == DART_CAMERA_E_NO_FRAME)
        return mp_const_none;
    raw_check(rc, "acquire");
    // No Python allocation/exception while a grayscale lease is held.
    // Return a brightness statistic without changing any grayscale pixels.
    uint32_t bright = 0;
    if (frame.width != 640 || frame.height != 480 || frame.stride != 640) {
        (void)dart_camera_release_frame(self->camera, &frame);
        mp_raise_ValueError(MP_ERROR_TEXT("unexpected camera frame geometry"));
    }
    memcpy(output.buf, frame.data, output.len);
    for (size_t i = 0; i < output.len; ++i)
        bright += frame.data[i] >= self->threshold;
    rc = dart_camera_release_frame(self->camera, &frame);
    raw_check(rc, "release");
    self->sequence = frame.token;
    self->received_us = frame.received_us;
    self->source_pts = frame.source_pts;
    self->flags = frame.flags;
    self->has_frame = true;
    return mp_obj_new_int_from_uint(bright);
}
static MP_DEFINE_CONST_FUN_OBJ_3(raw_snapshot_into_obj, raw_snapshot_into);

static mp_obj_t raw_frame_info(mp_obj_t self_in) {
    raw_camera_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->has_frame)
        mp_raise_ValueError(MP_ERROR_TEXT("no successful camera frame"));
    mp_obj_t values[] = {mp_obj_new_int_from_ull(self->sequence),
                         mp_obj_new_int_from_ull(self->received_us),
                         mp_obj_new_int_from_ull(self->source_pts),
                         mp_obj_new_int_from_uint(self->flags)};
    return mp_obj_new_tuple(4, values);
}
static MP_DEFINE_CONST_FUN_OBJ_1(raw_frame_info_obj, raw_frame_info);

static mp_obj_t raw_camera_close(mp_obj_t self_in) {
    raw_check(raw_close(MP_OBJ_TO_PTR(self_in)), "close");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(raw_camera_close_obj, raw_camera_close);

static mp_obj_t raw_camera_finalize(mp_obj_t self_in) {
    int rc = raw_close(MP_OBJ_TO_PTR(self_in));
    if (rc != DART_CAMERA_OK)
        mp_printf(&mp_plat_print, "RAW camera finalizer cleanup failed: %d\n", rc);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(raw_camera_finalize_obj, raw_camera_finalize);

static const mp_rom_map_elem_t raw_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_snapshot_into), MP_ROM_PTR(&raw_snapshot_into_obj)},
    {MP_ROM_QSTR(MP_QSTR_frame_info), MP_ROM_PTR(&raw_frame_info_obj)},
    {MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&raw_camera_close_obj)},
    {MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&raw_camera_finalize_obj)},
};
static MP_DEFINE_CONST_DICT(raw_locals, raw_locals_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    raw_camera_type, MP_QSTR_Camera, MP_TYPE_FLAG_NONE,
    make_new, raw_make_new, locals_dict, &raw_locals
);

static const mp_rom_map_elem_t raw_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__dart_raw)},
    {MP_ROM_QSTR(MP_QSTR_PIXEL_FORMAT), MP_ROM_QSTR(MP_QSTR_gray8)},
    {MP_ROM_QSTR(MP_QSTR_Camera), MP_ROM_PTR(&raw_camera_type)},
};
static MP_DEFINE_CONST_DICT(raw_globals, raw_globals_table);
const mp_obj_module_t dart_raw_module = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&raw_globals,
};
MP_REGISTER_MODULE(MP_QSTR__dart_raw, dart_raw_module);
