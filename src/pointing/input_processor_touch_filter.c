/*
 * Reduce trackball pointer jitter while keeping fast movement as is.
 *
 * - Touch filter: after the ball has been idle for idle-ms, movement is dropped until
 *   start-threshold counts accumulate (dropped events do not trigger the auto mouse layer).
 * - Low-speed gain: movement is multiplied by a gain that rises smoothly from
 *   slow-gain-percent (at or below slow-speed) to 100% (at or above full-speed).
 *   Speeds are counts per 16 ms. The same gain is used for X and Y of one report so the
 *   direction is preserved; it is derived from the speed of the preceding reports.
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

#define GAIN_ONE 256
#define SPEED_SHIFT 4
#define NOMINAL_REPORT_MS 16

struct touch_filter_config {
    int32_t idle_ms;
    int32_t start_threshold;
    int32_t slow_gain;
    int32_t slow_speed;
    int32_t full_speed;
};

struct touch_filter_data {
    int64_t last_event_ms;
    int64_t last_sync_ms;
    int32_t accumulated;
    int32_t speed;
    int32_t report_counts;
    int32_t gain;
    int32_t remainder[2];
    bool moving;
    bool in_report;
};

static int32_t gain_for_speed(const struct touch_filter_config *cfg, int32_t speed) {
    int32_t slow = cfg->slow_speed << SPEED_SHIFT;
    int32_t full = cfg->full_speed << SPEED_SHIFT;

    if (speed <= slow || full <= slow) {
        return speed >= full ? GAIN_ONE : cfg->slow_gain;
    }
    if (speed >= full) {
        return GAIN_ONE;
    }
    return cfg->slow_gain + (GAIN_ONE - cfg->slow_gain) * (speed - slow) / (full - slow);
}

static void reset_motion(struct touch_filter_data *data, int64_t now) {
    data->moving = false;
    data->in_report = false;
    data->accumulated = 0;
    data->speed = 0;
    data->report_counts = 0;
    data->remainder[0] = data->remainder[1] = 0;
    data->last_sync_ms = now - NOMINAL_REPORT_MS;
}

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
        reset_motion(data, now);
    }
    data->last_event_ms = now;

    if (!data->moving) {
        data->accumulated += abs(event->value);
        if (data->accumulated < cfg->start_threshold) {
            return ZMK_INPUT_PROC_STOP;
        }
        data->moving = true;
    }

    if (!data->in_report) {
        data->gain = gain_for_speed(cfg, data->speed);
        data->in_report = true;
    }

    data->report_counts += abs(event->value);

    int idx = event->code == INPUT_REL_X ? 0 : 1;
    int32_t value = event->value * data->gain + data->remainder[idx];
    int32_t scaled = value / GAIN_ONE;
    data->remainder[idx] = value - scaled * GAIN_ONE;
    event->value = (int16_t)CLAMP(scaled, INT16_MIN, INT16_MAX);

    if (event->sync) {
        int32_t dt = (int32_t)CLAMP(now - data->last_sync_ms, 4, 64);
        int32_t instant = (data->report_counts << SPEED_SHIFT) * NOMINAL_REPORT_MS / dt;
        data->speed = (data->speed + instant) / 2;
        data->last_sync_ms = now;
        data->report_counts = 0;
        data->in_report = false;
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

static struct zmk_input_processor_driver_api touch_filter_driver_api = {
    .handle_event = touch_filter_handle_event,
};

#define TOUCH_FILTER_INST(n)                                                                       \
    BUILD_ASSERT(DT_INST_PROP(n, slow_gain_percent) > 0 &&                                         \
                     DT_INST_PROP(n, slow_gain_percent) <= 100,                                    \
                 "slow-gain-percent must be 1..100");                                              \
    static struct touch_filter_data touch_filter_data_##n = {};                                    \
    static const struct touch_filter_config touch_filter_config_##n = {                            \
        .idle_ms = DT_INST_PROP(n, idle_ms),                                                       \
        .start_threshold = DT_INST_PROP(n, start_threshold),                                       \
        .slow_gain = DT_INST_PROP(n, slow_gain_percent) * GAIN_ONE / 100,                          \
        .slow_speed = DT_INST_PROP(n, slow_speed),                                                 \
        .full_speed = DT_INST_PROP(n, full_speed),                                                 \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &touch_filter_data_##n, &touch_filter_config_##n,         \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                        \
                          &touch_filter_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TOUCH_FILTER_INST)
