/*
 * Suppress pointer jitter from light touches on the trackball while keeping fast movement as is.
 *
 * - After the ball has been idle for idle-ms, movement is dropped until start-threshold counts
 *   accumulate (dropped events also do not trigger the auto mouse layer).
 * - Per-report movements of at most slow-max counts are divided by slow-divisor.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_touch_filter

#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <drivers/input_processor.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct touch_filter_config {
    int32_t idle_ms;
    int32_t start_threshold;
    int32_t slow_max;
    int32_t slow_divisor;
};

struct touch_filter_data {
    int64_t last_event_ms;
    int32_t accumulated;
    bool moving;
    int16_t remainder[2];
};

static int touch_filter_handle_event(const struct device *dev, struct input_event *event,
                                     uint32_t param1, uint32_t param2,
                                     struct zmk_input_processor_state *state) {
    const struct touch_filter_config *cfg = dev->config;
    struct touch_filter_data *data = dev->data;

    if (event->type != INPUT_EV_REL ||
        (event->code != INPUT_REL_X && event->code != INPUT_REL_Y)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    int64_t now = k_uptime_get();
    if (now - data->last_event_ms > cfg->idle_ms) {
        data->moving = false;
        data->accumulated = 0;
        data->remainder[0] = data->remainder[1] = 0;
    }
    data->last_event_ms = now;

    if (!data->moving) {
        data->accumulated += abs(event->value);
        if (data->accumulated < cfg->start_threshold) {
            return ZMK_INPUT_PROC_STOP;
        }
        data->moving = true;
    }

    int idx = event->code == INPUT_REL_X ? 0 : 1;
    if (cfg->slow_divisor > 1 && abs(event->value) <= cfg->slow_max) {
        int32_t value = event->value + data->remainder[idx];
        int32_t scaled = value / cfg->slow_divisor;
        data->remainder[idx] = value - scaled * cfg->slow_divisor;
        event->value = scaled;
    } else {
        data->remainder[idx] = 0;
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

static struct zmk_input_processor_driver_api touch_filter_driver_api = {
    .handle_event = touch_filter_handle_event,
};

#define TOUCH_FILTER_INST(n)                                                                       \
    static struct touch_filter_data touch_filter_data_##n = {};                                    \
    static const struct touch_filter_config touch_filter_config_##n = {                            \
        .idle_ms = DT_INST_PROP(n, idle_ms),                                                       \
        .start_threshold = DT_INST_PROP(n, start_threshold),                                       \
        .slow_max = DT_INST_PROP(n, slow_max),                                                     \
        .slow_divisor = DT_INST_PROP(n, slow_divisor),                                             \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &touch_filter_data_##n, &touch_filter_config_##n,         \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                        \
                          &touch_filter_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TOUCH_FILTER_INST)
