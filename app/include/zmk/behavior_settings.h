/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/device.h>
#include <drivers/behavior.h>

/**
 * @brief Get the runtime settings a behavior instance offers.
 *
 * @retval 0 and sets @p settings / @p len (len is 0 when the behavior has none).
 */
int zmk_behavior_settings_get_metadata(const struct device *dev,
                                       const struct behavior_setting_metadata **settings,
                                       size_t *len);

int zmk_behavior_settings_get(const struct device *dev, uint8_t id, int32_t *value,
                              int32_t *default_value);

/**
 * @brief Change a setting right away. It is kept until saved or discarded.
 *
 * @retval -ENOTSUP the behavior has no runtime settings
 * @retval -ENOENT no setting with that ID
 * @retval -EINVAL the value is out of range for the setting
 */
int zmk_behavior_settings_set(const struct device *dev, uint8_t id, int32_t value);

bool zmk_behavior_settings_check_unsaved_changes(void);
int zmk_behavior_settings_save_changes(void);
int zmk_behavior_settings_discard_changes(void);

/** @brief Put every setting back to its devicetree default and delete the saved values. */
int zmk_behavior_settings_reset(void);
