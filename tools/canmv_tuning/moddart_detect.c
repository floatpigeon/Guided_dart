#include "py/runtime.h"
#include "detector_bridge.h"

typedef struct {
    mp_obj_base_t base;
    dart_tuning_detector *detector;
} detector_obj;

static mp_obj_t detector_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw,
                             const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);
    if (!mp_obj_is_type(args[0], &mp_type_dict))
        mp_raise_TypeError(MP_ERROR_TEXT("Detector config must be a dict"));
    mp_map_t *map = mp_obj_dict_get_map(args[0]);
    if (map->used > 80)
        mp_raise_ValueError(MP_ERROR_TEXT("at most 80 detector parameters"));
    dart_tuning_parameter parameters[80];
    size_t count = 0;
    for (size_t i = 0; i < map->alloc; ++i) {
        if (!mp_map_slot_is_filled(map, i)) continue;
        parameters[count].name = mp_obj_str_get_str(map->table[i].key);
        parameters[count].value = mp_obj_get_float(map->table[i].value);
        ++count;
    }
    detector_obj *self = m_new_obj_with_finaliser(detector_obj);
    self->base.type = type;
    self->detector = NULL;
    size_t bad_parameter;
    int rc = dart_tuning_detector_create(parameters, count, &self->detector, &bad_parameter);
    if (rc != 0) {
        if (bad_parameter < count)
            mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("invalid detector parameter: %s"),
                             parameters[bad_parameter].name);
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("Detector prepare failed: %d"), rc);
    }
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t rect_tuple(dart_tuning_rect value) {
    mp_obj_t values[] = {mp_obj_new_int_from_uint(value.x), mp_obj_new_int_from_uint(value.y),
                         mp_obj_new_int_from_uint(value.width), mp_obj_new_int_from_uint(value.height)};
    return mp_obj_new_tuple(4, values);
}

static void store(mp_obj_t dict, qstr key, mp_obj_t value) {
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), value);
}

static mp_obj_t target_dict(dart_tuning_target value) {
    if (!value.valid) return mp_const_none;
    mp_obj_t result = mp_obj_new_dict(12);
    mp_obj_t center[] = {mp_obj_new_float(value.x), mp_obj_new_float(value.y)};
    store(result, MP_QSTR_center, mp_obj_new_tuple(2, center));
    store(result, MP_QSTR_bounds, rect_tuple(value.bounds));
    store(result, MP_QSTR_area, mp_obj_new_int_from_uint(value.area));
    store(result, MP_QSTR_radius, mp_obj_new_float(value.radius));
    store(result, MP_QSTR_quality, mp_obj_new_float(value.quality));
    store(result, MP_QSTR_clipped, mp_obj_new_bool(value.clipped));
    store(result, MP_QSTR_contrast, mp_obj_new_float(value.contrast));
    store(result, MP_QSTR_snr, mp_obj_new_float(value.snr));
    store(result, MP_QSTR_stability, mp_obj_new_float(value.stability));
    store(result, MP_QSTR_stability_checked, mp_obj_new_bool(value.stability_checked));
    store(result, MP_QSTR_brightness_ratio, mp_obj_new_float(value.brightness_ratio));
    store(result, MP_QSTR_gap_contrast, mp_obj_new_float(value.gap_contrast));
    return result;
}

static mp_obj_t detector_detect(mp_obj_t self_in, mp_obj_t buffer_in, mp_obj_t metadata_in) {
    detector_obj *self = MP_OBJ_TO_PTR(self_in);
    if (self->detector == NULL)
        mp_raise_ValueError(MP_ERROR_TEXT("detector is closed"));
    mp_buffer_info_t pixels;
    mp_get_buffer_raise(buffer_in, &pixels, MP_BUFFER_READ);
    mp_obj_t *metadata;
    mp_obj_get_array_fixed_n(metadata_in, 4, &metadata);
    mp_int_t values[4];
    for (size_t i = 0; i < 4; ++i) {
        values[i] = mp_obj_get_int(metadata[i]);
        if (values[i] < 0)
            mp_raise_ValueError(MP_ERROR_TEXT("frame metadata must be nonnegative"));
    }
    dart_tuning_detection detected;
    int rc = dart_tuning_detector_detect(self->detector, pixels.buf, pixels.len,
                                        values[0], values[1], values[2], values[3], &detected);
    if (rc != 0)
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("Detector detect failed: %d"), rc);
    mp_obj_t result = mp_obj_new_dict(16);
    store(result, MP_QSTR_kind, MP_OBJ_NEW_QSTR(detected.kind == 2 ? MP_QSTR_armor
                                             : detected.kind == 1 ? MP_QSTR_light : MP_QSTR_none));
    store(result, MP_QSTR_target, target_dict(detected.target));
    store(result, MP_QSTR_light, target_dict(detected.light));
    store(result, MP_QSTR_armor, target_dict(detected.armor));
    store(result, MP_QSTR_roi, rect_tuple(detected.window));
    store(result, MP_QSTR_full_frame, mp_obj_new_bool(detected.full_frame));
    store(result, MP_QSTR_rescanned, mp_obj_new_bool(detected.rescanned));
    store(result, MP_QSTR_candidates, mp_obj_new_int_from_uint(detected.candidates));
    store(result, MP_QSTR_missed_frames, mp_obj_new_int_from_uint(detected.missed_frames));
    store(result, MP_QSTR_run_high_water, mp_obj_new_int_from_uint(detected.run_high_water));
    store(result, MP_QSTR_rejected_contrast, mp_obj_new_int_from_uint(detected.rejected_contrast));
    store(result, MP_QSTR_rejected_snr, mp_obj_new_int_from_uint(detected.rejected_snr));
    store(result, MP_QSTR_rejected_stability, mp_obj_new_int_from_uint(detected.rejected_stability));
    store(result, MP_QSTR_phase, MP_OBJ_NEW_QSTR(detected.phase == 1 ? MP_QSTR_track : MP_QSTR_detect));
    store(result, MP_QSTR_full_validation, mp_obj_new_bool(detected.full_validation));
    store(result, MP_QSTR_stability_checks, mp_obj_new_int_from_uint(detected.stability_checks));
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_3(detector_detect_obj, detector_detect);

static mp_obj_t detector_close(mp_obj_t self_in) {
    detector_obj *self = MP_OBJ_TO_PTR(self_in);
    dart_tuning_detector_destroy(self->detector);
    self->detector = NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(detector_close_obj, detector_close);

static mp_obj_t detector_reset(mp_obj_t self_in) {
    detector_obj *self = MP_OBJ_TO_PTR(self_in);
    if (self->detector == NULL)
        mp_raise_ValueError(MP_ERROR_TEXT("detector is closed"));
    dart_tuning_detector_reset(self->detector);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(detector_reset_obj, detector_reset);

static const mp_rom_map_elem_t detector_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_detect), MP_ROM_PTR(&detector_detect_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&detector_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&detector_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&detector_close_obj) },
};
static MP_DEFINE_CONST_DICT(detector_locals, detector_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(
    detector_type, MP_QSTR_Detector, MP_TYPE_FLAG_NONE,
    make_new, detector_new, locals_dict, &detector_locals
);

static const mp_rom_map_elem_t detector_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__dart_detect) },
    { MP_ROM_QSTR(MP_QSTR_Detector), MP_ROM_PTR(&detector_type) },
    { MP_ROM_QSTR(MP_QSTR_GRAY_FEATURE_VERSION), MP_ROM_INT(1) },
    { MP_ROM_QSTR(MP_QSTR_TRACK_FEATURE_VERSION), MP_ROM_INT(1) },
};
static MP_DEFINE_CONST_DICT(detector_globals, detector_globals_table);
const mp_obj_module_t dart_detect_module = {
    .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&detector_globals,
};
MP_REGISTER_MODULE(MP_QSTR__dart_detect, dart_detect_module);
