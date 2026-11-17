/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/behavior_settings.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Saved values live at "bhv_set/<behavior device name>/<setting id>". The device name is
// used rather than the local ID, because with the settings-table local ID type those IDs
// are themselves only known once settings have loaded.
#define SETTINGS_ROOT "bhv_set"
#define SETTINGS_KEY_FMT SETTINGS_ROOT "/%s/%d"

static bool unsaved_changes;

static const struct behavior_driver_api *api_for(const struct device *dev) {
    return dev ? (const struct behavior_driver_api *)dev->api : NULL;
}

static bool has_settings(const struct behavior_driver_api *api) {
    return api && api->settings_len > 0 && api->get_setting && api->set_setting;
}

static const struct behavior_setting_metadata *find_setting(const struct behavior_driver_api *api,
                                                            uint8_t id) {
    for (size_t i = 0; i < api->settings_len; i++) {
        if (api->settings[i].id == id) {
            return &api->settings[i];
        }
    }
    return NULL;
}

static bool value_is_valid(const struct behavior_setting_metadata *meta, int32_t value) {
    switch (meta->type) {
    case BEHAVIOR_SETTING_TYPE_RANGE:
        return value >= meta->min && value <= meta->max;
    case BEHAVIOR_SETTING_TYPE_OPTIONS:
        return value >= 0 && value < (int32_t)meta->options_len;
    case BEHAVIOR_SETTING_TYPE_TOGGLE:
        return value == 0 || value == 1;
    default:
        return false;
    }
}

int zmk_behavior_settings_get_metadata(const struct device *dev,
                                       const struct behavior_setting_metadata **settings,
                                       size_t *len) {
    const struct behavior_driver_api *api = api_for(dev);
    if (!api) {
        return -EINVAL;
    }

    if (!has_settings(api)) {
        *settings = NULL;
        *len = 0;
        return 0;
    }

    *settings = api->settings;
    *len = api->settings_len;
    return 0;
}

int zmk_behavior_settings_get(const struct device *dev, uint8_t id, int32_t *value,
                              int32_t *default_value) {
    const struct behavior_driver_api *api = api_for(dev);
    if (!has_settings(api)) {
        return -ENOTSUP;
    }
    if (!find_setting(api, id)) {
        return -ENOENT;
    }
    return api->get_setting(dev, id, value, default_value);
}

static int apply_setting(const struct device *dev, uint8_t id, int32_t value) {
    const struct behavior_driver_api *api = api_for(dev);
    if (!has_settings(api)) {
        return -ENOTSUP;
    }

    const struct behavior_setting_metadata *meta = find_setting(api, id);
    if (!meta) {
        return -ENOENT;
    }
    if (!value_is_valid(meta, value)) {
        return -EINVAL;
    }

    return api->set_setting(dev, id, value);
}

int zmk_behavior_settings_set(const struct device *dev, uint8_t id, int32_t value) {
    int32_t current, def;
    int ret = zmk_behavior_settings_get(dev, id, &current, &def);
    if (ret < 0) {
        return ret;
    }

    ret = apply_setting(dev, id, value);
    if (ret < 0) {
        return ret;
    }

    if (current != value) {
        unsaved_changes = true;
    }
    return 0;
}

bool zmk_behavior_settings_check_unsaved_changes(void) { return unsaved_changes; }

typedef int (*setting_visitor_t)(const struct device *dev,
                                 const struct behavior_setting_metadata *meta);

static int foreach_setting(setting_visitor_t visit) {
    STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, item) {
        const struct behavior_driver_api *api = api_for(item->device);
        if (!device_is_ready(item->device) || !has_settings(api)) {
            continue;
        }

        for (size_t i = 0; i < api->settings_len; i++) {
            int ret = visit(item->device, &api->settings[i]);
            if (ret < 0) {
                return ret;
            }
        }
    }
    return 0;
}

static void settings_key(char *buf, size_t len, const struct device *dev, uint8_t id) {
    snprintf(buf, len, SETTINGS_KEY_FMT, dev->name, id);
}

static int save_setting(const struct device *dev, const struct behavior_setting_metadata *meta) {
    int32_t value, def;
    int ret = zmk_behavior_settings_get(dev, meta->id, &value, &def);
    if (ret < 0) {
        return ret;
    }

    char key[SETTINGS_MAX_NAME_LEN + 1];
    settings_key(key, sizeof(key), dev, meta->id);

    // Only values that differ from the devicetree default take up storage
    if (value == def) {
        ret = settings_delete(key);
    } else {
        ret = settings_save_one(key, &value, sizeof(value));
    }

    if (ret < 0) {
        LOG_ERR("Failed to save behavior setting %s (%d)", key, ret);
    }
    return ret;
}

static int restore_default(const struct device *dev, const struct behavior_setting_metadata *meta) {
    int32_t value, def;
    int ret = zmk_behavior_settings_get(dev, meta->id, &value, &def);
    if (ret < 0) {
        return ret;
    }
    return apply_setting(dev, meta->id, def);
}

static int delete_saved(const struct device *dev, const struct behavior_setting_metadata *meta) {
    char key[SETTINGS_MAX_NAME_LEN + 1];
    settings_key(key, sizeof(key), dev, meta->id);
    int ret = settings_delete(key);
    return ret == -ENOENT ? 0 : ret;
}

int zmk_behavior_settings_save_changes(void) {
    if (!unsaved_changes) {
        return 0;
    }

    int ret = foreach_setting(save_setting);
    if (ret >= 0) {
        unsaved_changes = false;
    }
    return ret;
}

int zmk_behavior_settings_discard_changes(void) {
    if (!unsaved_changes) {
        return 0;
    }

    int ret = foreach_setting(restore_default);
    if (ret < 0) {
        return ret;
    }

    ret = settings_load_subtree(SETTINGS_ROOT);
    if (ret >= 0) {
        unsaved_changes = false;
    }
    return ret;
}

int zmk_behavior_settings_reset(void) {
    int ret = foreach_setting(delete_saved);
    if (ret < 0) {
        return ret;
    }

    ret = foreach_setting(restore_default);
    if (ret >= 0) {
        unsaved_changes = false;
    }
    return ret;
}

static int behavior_settings_handle_set(const char *name, size_t len, settings_read_cb read_cb,
                                        void *cb_arg) {
    const char *next;
    int name_len = settings_name_next(name, &next);
    if (!next || name_len <= 0 || name_len >= 64) {
        LOG_WRN("Unexpected behavior setting %s", name);
        return -EINVAL;
    }

    char dev_name[64];
    memcpy(dev_name, name, name_len);
    dev_name[name_len] = '\0';

    char *endptr;
    unsigned long id = strtoul(next, &endptr, 10);
    if (endptr == next || *endptr != '\0' || id > UINT8_MAX) {
        LOG_WRN("Unexpected behavior setting %s", name);
        return -EINVAL;
    }

    int32_t value;
    if (len != sizeof(value)) {
        LOG_WRN("Behavior setting %s has the wrong size %d", name, len);
        return -EINVAL;
    }

    int ret = read_cb(cb_arg, &value, sizeof(value));
    if (ret < 0) {
        return ret;
    }

    const struct device *dev = zmk_behavior_get_binding(dev_name);
    if (!dev) {
        // The behavior was removed from the keymap since the setting was saved
        LOG_WRN("No behavior %s for saved setting %lu", dev_name, id);
        return 0;
    }

    ret = apply_setting(dev, (uint8_t)id, value);
    if (ret < 0) {
        LOG_WRN("Could not apply saved setting %lu of %s (%d)", id, dev_name, ret);
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(behavior_settings, SETTINGS_ROOT, NULL, behavior_settings_handle_set,
                               NULL, NULL);
