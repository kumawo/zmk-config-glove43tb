/*
 * Activate the macOS layer automatically depending on the selected endpoint
 * (BLE profile / USB), so that one keymap works for both Windows and macOS hosts.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>

#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/events/ble_active_profile_changed.h>
#endif

#define OS_LAYER CONFIG_GLOVE43TB_OS_LAYER_INDEX

static bool os_layer_wanted;

static bool endpoint_wants_os_layer(struct zmk_endpoint_instance endpoint) {
    switch (endpoint.transport) {
    case ZMK_TRANSPORT_USB:
        return IS_ENABLED(CONFIG_GLOVE43TB_OS_LAYER_USB);
    case ZMK_TRANSPORT_BLE:
        return endpoint.ble.profile_index >= 0 && endpoint.ble.profile_index < 32 &&
               (CONFIG_GLOVE43TB_OS_LAYER_BLE_PROFILES & BIT(endpoint.ble.profile_index));
    }

    return false;
}

static void os_layer_update(struct k_work *work) {
    os_layer_wanted = endpoint_wants_os_layer(zmk_endpoints_selected());

    if (os_layer_wanted) {
        zmk_keymap_layer_activate(OS_LAYER);
    } else {
        zmk_keymap_layer_deactivate(OS_LAYER);
    }
}

static K_WORK_DELAYABLE_DEFINE(os_layer_work, os_layer_update);

static int os_layer_listener(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *layer_ev = as_zmk_layer_state_changed(eh);

    if (layer_ev) {
        // `&to` deactivates every layer, so turn the OS layer back on while it is wanted.
        if (layer_ev->layer == OS_LAYER && !layer_ev->state && os_layer_wanted) {
            zmk_keymap_layer_activate(OS_LAYER);
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    // Defer so that the endpoints module has already updated the selected endpoint.
    k_work_reschedule(&os_layer_work, K_MSEC(10));
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(glove43tb_os_layer, os_layer_listener);
ZMK_SUBSCRIPTION(glove43tb_os_layer, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(glove43tb_os_layer, zmk_layer_state_changed);
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(glove43tb_os_layer, zmk_ble_active_profile_changed);
#endif

static int os_layer_init(void) {
    k_work_schedule(&os_layer_work, K_SECONDS(1));
    return 0;
}

SYS_INIT(os_layer_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
