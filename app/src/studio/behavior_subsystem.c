/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <pb_encode.h>
#include <zmk/studio/rpc.h>
#include <drivers/behavior.h>
#include <zmk/hid.h>

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_RUNTIME_SETTINGS)
#include <zmk/behavior_settings.h>
#endif

ZMK_RPC_SUBSYSTEM(behaviors)

#define BEHAVIOR_RESPONSE(type, ...) ZMK_RPC_RESPONSE(behaviors, type, __VA_ARGS__)

static bool encode_behavior_summaries(pb_ostream_t *stream, const pb_field_t *field,
                                      void *const *arg) {
    STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, beh) {
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        if (!pb_encode_varint(stream, beh->local_id)) {
            LOG_ERR("Failed to encode behavior ID");
            return false;
        }
    }

    return true;
}

zmk_studio_Response list_all_behaviors(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_behaviors_ListAllBehaviorsResponse beh_resp =
        zmk_behaviors_ListAllBehaviorsResponse_init_zero;
    beh_resp.behaviors.funcs.encode = encode_behavior_summaries;

    return BEHAVIOR_RESPONSE(list_all_behaviors, beh_resp);
}

struct encode_metadata_sets_state {
    const struct behavior_parameter_metadata_set *sets;
    size_t sets_len;
    size_t i;
};

static bool encode_value_description_name(pb_ostream_t *stream, const pb_field_t *field,
                                          void *const *arg) {
    struct behavior_parameter_value_metadata *state =
        (struct behavior_parameter_value_metadata *)*arg;

    if (!state->display_name) {
        return true;
    }

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, state->display_name, strlen(state->display_name));
}

static bool encode_value_description(pb_ostream_t *stream, const pb_field_t *field,
                                     void *const *arg) {
    struct encode_metadata_sets_state *state = (struct encode_metadata_sets_state *)*arg;

    const struct behavior_parameter_metadata_set *set = &state->sets[state->i];

    bool is_param1 = field->tag == zmk_behaviors_BehaviorBindingParametersSet_param1_tag;
    size_t values_len = is_param1 ? set->param1_values_len : set->param2_values_len;
    const struct behavior_parameter_value_metadata *values =
        is_param1 ? set->param1_values : set->param2_values;

    for (int val_i = 0; val_i < values_len; val_i++) {
        const struct behavior_parameter_value_metadata *val = &values[val_i];

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        zmk_behaviors_BehaviorParameterValueDescription desc =
            zmk_behaviors_BehaviorParameterValueDescription_init_zero;
        desc.name.funcs.encode = encode_value_description_name;
        desc.name.arg = (void *)val;

        switch (val->type) {
        case BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE:
            desc.which_value_type = zmk_behaviors_BehaviorParameterValueDescription_constant_tag;
            desc.value_type.constant = val->value;
            break;
        case BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE:
            desc.which_value_type = zmk_behaviors_BehaviorParameterValueDescription_range_tag;
            desc.value_type.range.min = val->range.min;
            desc.value_type.range.max = val->range.max;
            break;
        case BEHAVIOR_PARAMETER_VALUE_TYPE_NIL:
            desc.which_value_type = zmk_behaviors_BehaviorParameterValueDescription_nil_tag;
            break;
        case BEHAVIOR_PARAMETER_VALUE_TYPE_HID_USAGE:
            desc.which_value_type = zmk_behaviors_BehaviorParameterValueDescription_hid_usage_tag;
            desc.value_type.hid_usage.consumer_max = ZMK_HID_CONSUMER_MAX_USAGE;
            desc.value_type.hid_usage.keyboard_max = ZMK_HID_KEYBOARD_MAX_USAGE;
            break;
        case BEHAVIOR_PARAMETER_VALUE_TYPE_LAYER_ID:
            desc.which_value_type = zmk_behaviors_BehaviorParameterValueDescription_layer_id_tag;
            break;
        default:
            LOG_ERR("Unknown value description type %d", val->type);
            return false;
        }

        if (!pb_encode_submessage(stream, &zmk_behaviors_BehaviorParameterValueDescription_msg,
                                  &desc)) {
            LOG_WRN("Failed to encode submessage for set %d, value %d!", state->i, val_i);
            return false;
        }
    }

    return true;
}

static bool encode_metadata_sets(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    struct encode_metadata_sets_state *state = (struct encode_metadata_sets_state *)*arg;
    bool ret = true;

    LOG_DBG("Encoding the %d metadata sets with %p", state->sets_len, state->sets);

    for (int i = 0; i < state->sets_len; i++) {
        LOG_DBG("Encoding set %d", i);
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        state->i = i;
        zmk_behaviors_BehaviorBindingParametersSet msg =
            zmk_behaviors_BehaviorBindingParametersSet_init_zero;
        msg.param1.funcs.encode = encode_value_description;
        msg.param1.arg = state;
        msg.param2.funcs.encode = encode_value_description;
        msg.param2.arg = state;
        ret = pb_encode_submessage(stream, &zmk_behaviors_BehaviorBindingParametersSet_msg, &msg);
        if (!ret) {
            LOG_WRN("Failed to encode submessage for set %d", i);
            break;
        }
    }

    return ret;
}

static bool encode_behavior_name(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    struct zmk_behavior_ref *zbm = (struct zmk_behavior_ref *)*arg;

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, zbm->metadata.display_name, strlen(zbm->metadata.display_name));
}

static struct encode_metadata_sets_state state = {};

zmk_studio_Response get_behavior_details(const zmk_studio_Request *req) {
    uint32_t behavior_id = req->subsystem.behaviors.request_type.get_behavior_details.behavior_id;
    const char *behavior_name = zmk_behavior_find_behavior_name_from_local_id(behavior_id);

    LOG_DBG("behavior_id %d, name %s", behavior_id, behavior_name);

    if (!behavior_name) {
        LOG_WRN("No behavior found for ID %d", behavior_id);
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    const struct device *device = behavior_get_binding(behavior_name);

    struct zmk_behavior_ref *zbm = NULL;
    STRUCT_SECTION_FOREACH(zmk_behavior_ref, item) {
        if (item->device == device) {
            zbm = item;
            break;
        }
    }

    __ASSERT(zbm != NULL, "Can't find a device without also having metadata");

    struct behavior_parameter_metadata desc = {0};
    int ret = behavior_get_parameter_metadata(device, &desc);
    if (ret < 0) {
        LOG_DBG("Failed to fetch the metadata for %s! %d", zbm->metadata.display_name, ret);
    } else {
        LOG_DBG("Got metadata with %d sets", desc.sets_len);
    }

    zmk_behaviors_GetBehaviorDetailsResponse resp =
        zmk_behaviors_GetBehaviorDetailsResponse_init_zero;
    resp.id = behavior_id;
    resp.display_name.funcs.encode = encode_behavior_name;
    resp.display_name.arg = zbm;

    state.sets = desc.sets;
    state.sets_len = desc.sets_len;

    resp.metadata.funcs.encode = encode_metadata_sets;
    resp.metadata.arg = &state;

    return BEHAVIOR_RESPONSE(get_behavior_details, resp);
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_RUNTIME_SETTINGS)

static const struct device *device_for_local_id(uint32_t behavior_id) {
    const char *name = zmk_behavior_find_behavior_name_from_local_id(behavior_id);
    return name ? zmk_behavior_get_binding(name) : NULL;
}

static bool encode_string_arg(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const char *str = (const char *)*arg;

    if (!str) {
        return true;
    }

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, str, strlen(str));
}

static bool encode_setting_options(pb_ostream_t *stream, const pb_field_t *field,
                                   void *const *arg) {
    const struct behavior_setting_metadata *meta = (const struct behavior_setting_metadata *)*arg;

    for (size_t i = 0; i < meta->options_len; i++) {
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        if (!pb_encode_string(stream, meta->options[i], strlen(meta->options[i]))) {
            return false;
        }
    }

    return true;
}

static bool encode_behavior_settings(pb_ostream_t *stream, const pb_field_t *field,
                                     void *const *arg) {
    const struct device *dev = (const struct device *)*arg;
    const struct behavior_setting_metadata *settings;
    size_t len;

    if (zmk_behavior_settings_get_metadata(dev, &settings, &len) < 0) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        const struct behavior_setting_metadata *meta = &settings[i];
        int32_t value, default_value;

        if (zmk_behavior_settings_get(dev, meta->id, &value, &default_value) < 0) {
            LOG_WRN("Failed to read setting %d of %s", meta->id, dev->name);
            continue;
        }

        zmk_behaviors_BehaviorSetting setting = zmk_behaviors_BehaviorSetting_init_zero;
        setting.id = meta->id;
        setting.name.funcs.encode = encode_string_arg;
        setting.name.arg = (void *)meta->display_name;
        setting.value = value;
        setting.default_value = default_value;

        switch (meta->type) {
        case BEHAVIOR_SETTING_TYPE_RANGE:
            setting.which_value_type = zmk_behaviors_BehaviorSetting_range_tag;
            setting.value_type.range.min = meta->min;
            setting.value_type.range.max = meta->max;
            setting.value_type.range.unit.funcs.encode = encode_string_arg;
            setting.value_type.range.unit.arg = (void *)meta->unit;
            break;
        case BEHAVIOR_SETTING_TYPE_OPTIONS:
            setting.which_value_type = zmk_behaviors_BehaviorSetting_options_tag;
            setting.value_type.options.options.funcs.encode = encode_setting_options;
            setting.value_type.options.options.arg = (void *)meta;
            break;
        case BEHAVIOR_SETTING_TYPE_TOGGLE:
            setting.which_value_type = zmk_behaviors_BehaviorSetting_toggle_tag;
            break;
        default:
            LOG_ERR("Unknown setting type %d", meta->type);
            return false;
        }

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        if (!pb_encode_submessage(stream, &zmk_behaviors_BehaviorSetting_msg, &setting)) {
            return false;
        }
    }

    return true;
}

zmk_studio_Response get_behavior_settings(const zmk_studio_Request *req) {
    uint32_t behavior_id = req->subsystem.behaviors.request_type.get_behavior_settings.behavior_id;
    const struct device *dev = device_for_local_id(behavior_id);

    if (!dev) {
        LOG_WRN("No behavior found for ID %d", behavior_id);
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    zmk_behaviors_GetBehaviorSettingsResponse resp =
        zmk_behaviors_GetBehaviorSettingsResponse_init_zero;
    resp.behavior_id = behavior_id;
    resp.settings.funcs.encode = encode_behavior_settings;
    resp.settings.arg = (void *)dev;

    return BEHAVIOR_RESPONSE(get_behavior_settings, resp);
}

zmk_studio_Response set_behavior_setting(const zmk_studio_Request *req) {
    const zmk_behaviors_SetBehaviorSettingRequest *set_req =
        &req->subsystem.behaviors.request_type.set_behavior_setting;
    const struct device *dev = device_for_local_id(set_req->behavior_id);

    if (!dev) {
        return BEHAVIOR_RESPONSE(
            set_behavior_setting,
            zmk_behaviors_SetBehaviorSettingResponse_SET_BEHAVIOR_SETTING_RESP_INVALID_BEHAVIOR);
    }

    if (set_req->setting_id > UINT8_MAX) {
        return BEHAVIOR_RESPONSE(
            set_behavior_setting,
            zmk_behaviors_SetBehaviorSettingResponse_SET_BEHAVIOR_SETTING_RESP_INVALID_SETTING);
    }

    int ret = zmk_behavior_settings_set(dev, (uint8_t)set_req->setting_id, set_req->value);
    switch (ret) {
    case 0:
        break;
    case -EINVAL:
        return BEHAVIOR_RESPONSE(
            set_behavior_setting,
            zmk_behaviors_SetBehaviorSettingResponse_SET_BEHAVIOR_SETTING_RESP_INVALID_VALUE);
    case -ENOTSUP:
        return BEHAVIOR_RESPONSE(
            set_behavior_setting,
            zmk_behaviors_SetBehaviorSettingResponse_SET_BEHAVIOR_SETTING_RESP_INVALID_BEHAVIOR);
    default:
        return BEHAVIOR_RESPONSE(
            set_behavior_setting,
            zmk_behaviors_SetBehaviorSettingResponse_SET_BEHAVIOR_SETTING_RESP_INVALID_SETTING);
    }

    if (zmk_behavior_settings_check_unsaved_changes()) {
        raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
            .notification = ZMK_RPC_NOTIFICATION(keymap, unsaved_changes_status_changed, true)});
    }

    return BEHAVIOR_RESPONSE(
        set_behavior_setting,
        zmk_behaviors_SetBehaviorSettingResponse_SET_BEHAVIOR_SETTING_RESP_OK);
}

static int behavior_settings_reset(void) { return zmk_behavior_settings_reset(); }

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(behaviors, behavior_settings_reset);

ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, get_behavior_settings, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, set_behavior_setting, ZMK_STUDIO_RPC_HANDLER_SECURED);

#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_RUNTIME_SETTINGS)

ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, list_all_behaviors, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, get_behavior_details, ZMK_STUDIO_RPC_HANDLER_SECURED);
