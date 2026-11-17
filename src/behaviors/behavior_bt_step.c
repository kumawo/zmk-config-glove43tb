/*
 * Select the next / previous BLE profile, ignoring repeated triggers within cooldown-ms.
 * Intended for rotary encoders, where one detent may produce more than one trigger.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_bt_step

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/ble.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define BT_STEP_PREV 0
#define BT_STEP_NEXT 1

struct behavior_bt_step_config {
    int32_t cooldown_ms;
};

struct behavior_bt_step_data {
    int64_t last_switch_ms;
    bool switched;
};

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_parameter_value_metadata param_values[] = {
    {
        .display_name = "Previous Profile",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = BT_STEP_PREV,
    },
    {
        .display_name = "Next Profile",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = BT_STEP_NEXT,
    },
};

static const struct behavior_parameter_metadata_set param_metadata_set[] = {{
    .param1_values = param_values,
    .param1_values_len = ARRAY_SIZE(param_values),
}};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(param_metadata_set),
    .sets = param_metadata_set,
};

#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static int on_bt_step_binding_pressed(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_bt_step_config *cfg = dev->config;
    struct behavior_bt_step_data *data = dev->data;
    int64_t now = k_uptime_get();

    if (data->switched && now - data->last_switch_ms < cfg->cooldown_ms) {
        return ZMK_BEHAVIOR_OPAQUE;
    }

    data->switched = true;
    data->last_switch_ms = now;

    switch (binding->param1) {
    case BT_STEP_PREV:
        return zmk_ble_prof_prev();
    case BT_STEP_NEXT:
        return zmk_ble_prof_next();
    default:
        LOG_ERR("Unknown bt_step param: %d", binding->param1);
        return -ENOTSUP;
    }
}

static int on_bt_step_binding_released(struct zmk_behavior_binding *binding,
                                       struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_bt_step_driver_api = {
    .binding_pressed = on_bt_step_binding_pressed,
    .binding_released = on_bt_step_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

#define BT_STEP_INST(n)                                                                            \
    static struct behavior_bt_step_data behavior_bt_step_data_##n = {};                            \
    static const struct behavior_bt_step_config behavior_bt_step_config_##n = {                    \
        .cooldown_ms = DT_INST_PROP(n, cooldown_ms),                                               \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, &behavior_bt_step_data_##n,                             \
                            &behavior_bt_step_config_##n, POST_KERNEL,                             \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_bt_step_driver_api);

DT_INST_FOREACH_STATUS_OKAY(BT_STEP_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
