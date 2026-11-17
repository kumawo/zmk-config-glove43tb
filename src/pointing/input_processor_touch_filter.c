/*
 * Trackball pointer processing to reduce jitter while keeping fast movement as is.
 *
 * - Touch filter: after the ball has been idle for idle-ms, movement is dropped until
 *   start-threshold counts accumulate (dropped events do not trigger the auto mouse layer).
 * - Speed-dependent gain: the gain rises along an S-curve from the active preset's
 *   min-gain-percents value (at or below slow-speed) to 100% (at or above full-speed).
 *   Speeds are counts per 8 ms (one sensor frame). The same gain is applied to X and Y of a
 *   report so the direction is preserved; it is derived from the speed of preceding reports.
 * - The active preset can be changed at runtime (behavior_pointer_preset.c) and is persisted.
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

#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define GAIN_ONE 256
#define SPEED_SHIFT 4
#define FRAME_MS 8

struct touch_filter_config {
    int32_t idle_ms;
    int32_t start_threshold;
    int32_t slow_speed;
    int32_t full_speed;
    size_t presets_len;
    const int32_t *min_gain_percents;
};

struct touch_filter_data {
    int64_t last_event_ms;
    int64_t last_sync_ms;
    int32_t accumulated;
    int32_t speed;
    int32_t report_abs[2];
    int32_t gain;
    int32_t remainder[2];
    bool moving;
    bool in_report;
};

static uint8_t active_preset;

#if IS_ENABLED(CONFIG_SETTINGS)
static int pointer_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                void *cb_arg) {
    const char *next;

    if (settings_name_steq(name, "preset", &next) && !next) {
        if (len != sizeof(active_preset)) {
            return -EINVAL;
        }
        int rc = read_cb(cb_arg, &active_preset, sizeof(active_preset));
        return rc >= 0 ? 0 : rc;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(glove43tb_pointer, "glove43tb_ptr", NULL, pointer_settings_set,
                               NULL, NULL);

static void pointer_settings_save(struct k_work *work) {
    settings_save_one("glove43tb_ptr/preset", &active_preset, sizeof(active_preset));
}

static K_WORK_DELAYABLE_DEFINE(pointer_save_work, pointer_settings_save);
#endif

int glove43tb_pointer_preset_set(uint8_t preset) {
    active_preset = preset;
    LOG_INF("Pointer preset %d", preset);
#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_reschedule(&pointer_save_work, K_SECONDS(2));
#endif
    return 0;
}

static int32_t min_gain(const struct touch_filter_config *cfg) {
    uint8_t preset = active_preset < cfg->presets_len ? active_preset : 0;
    return cfg->min_gain_percents[preset] * GAIN_ONE / 100;
}

static int32_t gain_for_speed(const struct touch_filter_config *cfg, int32_t speed) {
    int32_t low = min_gain(cfg);
    int32_t slow = cfg->slow_speed << SPEED_SHIFT;
    int32_t full = cfg->full_speed << SPEED_SHIFT;

    if (low >= GAIN_ONE || speed >= full) {
        return GAIN_ONE;
    }
    if (speed <= slow || full <= slow) {
        return low;
    }

    // smoothstep: t^2 * (3 - 2t), t in [0, 1] as Q8
    int32_t t = (speed - slow) * GAIN_ONE / (full - slow);
    int32_t s = t * t / GAIN_ONE * (3 * GAIN_ONE - 2 * t) / GAIN_ONE;
    return low + (GAIN_ONE - low) * s / GAIN_ONE;
}

static void reset_motion(struct touch_filter_data *data, int64_t now) {
    data->moving = false;
    data->in_report = false;
    data->accumulated = 0;
    data->speed = 0;
    data->report_abs[0] = data->report_abs[1] = 0;
    data->remainder[0] = data->remainder[1] = 0;
    data->last_sync_ms = now - FRAME_MS;
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

    int idx = event->code == INPUT_REL_X ? 0 : 1;
    data->report_abs[idx] += abs(event->value);

    if (data->gain < GAIN_ONE) {
        int32_t value = event->value * data->gain + data->remainder[idx];
        int32_t scaled = value / GAIN_ONE;
        data->remainder[idx] = value - scaled * GAIN_ONE;
        event->value = (int16_t)CLAMP(scaled, INT16_MIN, INT16_MAX);
    } else {
        data->remainder[idx] = 0;
    }

    if (event->sync) {
        // Euclidean length approximated as max + min / 2
        int32_t hi = MAX(data->report_abs[0], data->report_abs[1]);
        int32_t lo = MIN(data->report_abs[0], data->report_abs[1]);
        int32_t dt = (int32_t)CLAMP(now - data->last_sync_ms, 2, 64);
        int32_t instant = ((hi + lo / 2) << SPEED_SHIFT) * FRAME_MS / dt;
        data->speed = (data->speed + instant) / 2;
        data->last_sync_ms = now;
        data->report_abs[0] = data->report_abs[1] = 0;
        data->in_report = false;
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

static struct zmk_input_processor_driver_api touch_filter_driver_api = {
    .handle_event = touch_filter_handle_event,
};

#define TOUCH_FILTER_INST(n)                                                                       \
    static const int32_t touch_filter_min_gains_##n[] = DT_INST_PROP(n, min_gain_percents);        \
    static struct touch_filter_data touch_filter_data_##n = {};                                    \
    static const struct touch_filter_config touch_filter_config_##n = {                            \
        .idle_ms = DT_INST_PROP(n, idle_ms),                                                       \
        .start_threshold = DT_INST_PROP(n, start_threshold),                                       \
        .slow_speed = DT_INST_PROP(n, slow_speed),                                                 \
        .full_speed = DT_INST_PROP(n, full_speed),                                                 \
        .presets_len = DT_INST_PROP_LEN(n, min_gain_percents),                                     \
        .min_gain_percents = touch_filter_min_gains_##n,                                           \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &touch_filter_data_##n, &touch_filter_config_##n,         \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                        \
                          &touch_filter_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TOUCH_FILTER_INST)
