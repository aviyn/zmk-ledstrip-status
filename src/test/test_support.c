/*
 * Test doubles for running the module on native_sim (see tests/).
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>

#include <dt-bindings/zmk/ledstrip_status_test.h>
#include <zmk/activity.h>
#include <zmk/behavior.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <dt-bindings/zmk/hid_indicators.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ----- led strip that logs each changed frame ----- */

#define DT_DRV_COMPAT zmk_ledstrip_status_test_strip

#define STRIP_NODE DT_INST(0, DT_DRV_COMPAT)
#define STRIP_LEN DT_PROP(STRIP_NODE, chain_length)
#define STRIP_HAS_SUPPLY DT_NODE_HAS_PROP(STRIP_NODE, supply_gpios)

#if STRIP_HAS_SUPPLY
static const struct gpio_dt_spec supply = GPIO_DT_SPEC_GET(STRIP_NODE, supply_gpios);
#endif

static char last_frame[STRIP_LEN * 7 + 1];

static int test_strip_update_rgb(const struct device *dev, struct led_rgb *pixels, size_t n) {
    char frame[sizeof(last_frame)] = "";
    const char *state = "on";

    for (size_t i = 0; i < n && i < STRIP_LEN; i++) {
        snprintf(frame + i * 7, 8, "%02x%02x%02x ", pixels[i].r, pixels[i].g, pixels[i].b);
    }
#if STRIP_HAS_SUPPLY
    // physical level; the supply switch is active low
    bool on = gpio_emul_output_get(supply.port, supply.pin) ==
              ((supply.dt_flags & GPIO_ACTIVE_LOW) ? 0 : 1);
    state = on ? "on" : "OFF";
#endif
    if (strcmp(frame, last_frame) != 0) {
        strcpy(last_frame, frame);
        LOG_DBG("t=%lld supply=%s %s", k_uptime_get(), state, frame);
    }
    return 0;
}

static size_t test_strip_length(const struct device *dev) { return STRIP_LEN; }

static DEVICE_API(led_strip, test_strip_api) = {
    .update_rgb = test_strip_update_rgb,
    .length = test_strip_length,
};

DEVICE_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_LED_STRIP_INIT_PRIORITY,
                      &test_strip_api);

/* ----- fuel gauge with a settable level ----- */

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT zmk_ledstrip_status_test_battery

static int battery_level = DT_INST_PROP(0, level);

static int test_battery_fetch(const struct device *dev, enum sensor_channel chan) { return 0; }

static int test_battery_get(const struct device *dev, enum sensor_channel chan,
                            struct sensor_value *val) {
    if (chan != SENSOR_CHAN_GAUGE_STATE_OF_CHARGE) {
        return -ENOTSUP;
    }
    *val = (struct sensor_value){.val1 = battery_level};
    return 0;
}

static DEVICE_API(sensor, test_battery_api) = {
    .sample_fetch = test_battery_fetch,
    .channel_get = test_battery_get,
};

DEVICE_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,
                      &test_battery_api);

/* ----- behavior that drives the test doubles ----- */

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT zmk_behavior_ledstrip_status_test

#define LST_NODE DT_INST(0, DT_DRV_COMPAT)

static const struct gpio_dt_spec vbus = GPIO_DT_SPEC_GET(LST_NODE, vbus_gpios);
static const struct gpio_dt_spec charge = GPIO_DT_SPEC_GET(LST_NODE, charge_gpios);

static void set_input(const struct gpio_dt_spec *spec, bool active) {
    bool level = (spec->dt_flags & GPIO_ACTIVE_LOW) ? !active : active;

    gpio_emul_input_set(spec->port, spec->pin, level);
}

static int lst_pressed(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event) {
    switch (binding->param1) {
    case LST_BATTERY:
        LOG_DBG("t=%lld battery %d %%", k_uptime_get(), binding->param2);
        battery_level = binding->param2;
        break;
    case LST_VBUS:
        LOG_DBG("t=%lld vbus %d", k_uptime_get(), binding->param2);
        set_input(&vbus, binding->param2);
        break;
    case LST_CHARGE:
        LOG_DBG("t=%lld charging %d", k_uptime_get(), binding->param2);
        set_input(&charge, binding->param2);
        break;
    case LST_SLEEP:
    case LST_WAKE:
        LOG_DBG("t=%lld %s", k_uptime_get(), binding->param1 == LST_SLEEP ? "sleep" : "wake");
        raise_zmk_activity_state_changed((struct zmk_activity_state_changed){
            .state = binding->param1 == LST_SLEEP ? ZMK_ACTIVITY_SLEEP : ZMK_ACTIVITY_ACTIVE});
        // sleep cuts the supply without a blank frame: log the next frame even if it repeats
        last_frame[0] = '\0';
        break;
    case LST_CAPS_LOCK:
        LOG_DBG("t=%lld caps lock %d", k_uptime_get(), binding->param2);
        raise_zmk_hid_indicators_changed((struct zmk_hid_indicators_changed){
            .indicators = binding->param2 ? HID_INDICATOR_CAPS_LOCK : 0});
        break;
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int lst_released(struct zmk_behavior_binding *binding,
                        struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api lst_api = {
    .binding_pressed = lst_pressed,
    .binding_released = lst_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &lst_api);
