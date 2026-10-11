/*
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_ledstrip_status

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>

#include <zmk_ledstrip_status/status.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct behavior_ledstrip_status_config {
    bool show_battery;
    bool show_connection;
};

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
#if IS_ENABLED(CONFIG_LEDSTRIP_STATUS)
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_ledstrip_status_config *cfg = dev->config;

    if (cfg->show_battery) {
        ledstrip_status_show_battery();
    }
    if (cfg->show_connection) {
        ledstrip_status_show_connection();
    }
#endif

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_ledstrip_status_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    // run on every half, so each one shows its own status
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

#define LSS_INST(n)                                                                                \
    static const struct behavior_ledstrip_status_config behavior_ledstrip_status_config_##n = {    \
        .show_battery = DT_INST_PROP(n, show_battery),                                             \
        .show_connection = DT_INST_PROP(n, show_connection),                                       \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, &behavior_ledstrip_status_config_##n,             \
                            POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                      \
                            &behavior_ledstrip_status_driver_api);

DT_INST_FOREACH_STATUS_OKAY(LSS_INST)
