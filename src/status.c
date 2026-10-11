/*
 * Battery, charging and connection status on a short addressable LED strip.
 *
 * Requests are queued and shown one after another by a dedicated thread. The
 * strip's supply is switched on only while something is shown, and cut again
 * (after blanking, so the data line idles low) once the queue is empty.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_ledstrip_status

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/activity.h>
#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/usb.h>

#if IS_ENABLED(CONFIG_SOC_SERIES_NRF52X)
#include <hal/nrf_power.h>
#endif

#include <zmk_ledstrip_status/status.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
             "LEDSTRIP_STATUS needs exactly one zmk,ledstrip-status node");

#define STATUS_NODE DT_DRV_INST(0)
#define STRIP_NODE DT_PHANDLE(STATUS_NODE, led_strip)
#define NUM_LEDS DT_PROP(STRIP_NODE, chain_length)

#define IS_CENTRAL (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
#define IS_BLE_PERIPHERAL (IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && !IS_CENTRAL)
#define HAS_BATTERY IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)

#define HAS_ENABLE DT_NODE_HAS_PROP(STATUS_NODE, enable_gpios)
#define HAS_CHARGE DT_NODE_HAS_PROP(STATUS_NODE, charge_gpios)

static const struct device *strip = DEVICE_DT_GET(STRIP_NODE);

#if HAS_ENABLE
static const struct gpio_dt_spec enable_gpio = GPIO_DT_SPEC_GET(STATUS_NODE, enable_gpios);
#endif
#if HAS_CHARGE
static const struct gpio_dt_spec charge_gpio = GPIO_DT_SPEC_GET(STATUS_NODE, charge_gpios);
static struct gpio_callback charge_cb;
#endif

#if DT_NODE_HAS_PROP(STATUS_NODE, led_order)
static const uint8_t led_order[] = DT_PROP(STATUS_NODE, led_order);
BUILD_ASSERT(ARRAY_SIZE(led_order) == NUM_LEDS, "led-order must list every LED of the strip");
#define STRIP_INDEX(i) (led_order[i])
#else
#define STRIP_INDEX(i) (i)
#endif

#define FULL 1000 // brightness scale, in permille of CONFIG_LEDSTRIP_STATUS_BRIGHTNESS
#define HALF (CONFIG_LEDSTRIP_STATUS_HALF_PERCENT * 10)
#define DIM (CONFIG_LEDSTRIP_STATUS_DIM_PERCENT * 10)

struct color {
    uint8_t r, g, b;
};

static const struct color RED = {255, 0, 0};
static const struct color AMBER = {255, 70, 0};
static const struct color YELLOW = {255, 170, 0};
static const struct color GREEN = {0, 255, 0};
static const struct color BLUE = {0, 0, 255};
static const struct color CYAN = {0, 200, 255};
static const struct color WHITE = {255, 255, 255};

enum display {
    DISPLAY_GAUGE,    // battery gauge
    DISPLAY_CHARGING, // battery gauge, highest lit LED breathing
    DISPLAY_CHARGED,  // all LEDs green
    DISPLAY_CRITICAL, // battery gauge, blinking
    DISPLAY_PROFILE,  // central: active BLE profile and its state
    DISPLAY_LINK,     // peripheral: split link connected (blue) or not (red)
};

struct request {
    uint8_t display;
    uint32_t duration_ms;
};

K_MSGQ_DEFINE(request_q, sizeof(struct request), 8, 4);

static struct led_rgb pixels[NUM_LEDS];

// guards `suspended` and the supply switch against the sleep handler
static struct k_spinlock power_lock;
static bool suspended;
static bool powered;

static bool initialized;

static void post(enum display display, uint32_t duration_ms) {
    struct request req = {.display = display, .duration_ms = duration_ms};

    if (suspended) {
        return;
    }
    if (k_msgq_put(&request_q, &req, K_NO_WAIT) < 0) {
        LOG_WRN("Status LED queue full, dropping display %d", display);
    }
}

/* ----- charger state ----- */

static bool usb_powered(void) {
#if defined(NRF_POWER_HAS_USBREG) && NRF_POWER_HAS_USBREG
    // works on split peripherals too, which have no USB stack
    return nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
#elif IS_ENABLED(CONFIG_ZMK_USB)
    return zmk_usb_is_powered();
#else
    return false;
#endif
}

static bool charging(void) {
#if HAS_CHARGE
    return usb_powered() && gpio_pin_get_dt(&charge_gpio) > 0;
#else
    return false;
#endif
}

/* ----- rendering ----- */

static void set_led(int i, struct color c, uint32_t permille) {
    uint32_t scale = CONFIG_LEDSTRIP_STATUS_BRIGHTNESS * permille;

    pixels[STRIP_INDEX(i)] = (struct led_rgb){
        .r = c.r * scale / (255 * FULL),
        .g = c.g * scale / (255 * FULL),
        .b = c.b * scale / (255 * FULL),
    };
}

static void clear_leds(void) { memset(pixels, 0, sizeof(pixels)); }

static bool blink_off(int64_t t) {
    return (t / (CONFIG_LEDSTRIP_STATUS_BLINK_PERIOD_MS / 2)) & 1;
}

// triangle wave from 15 % to 100 %, squared so it looks even to the eye
static uint32_t breathe(int64_t t) {
    const uint32_t period = CONFIG_LEDSTRIP_STATUS_BREATHE_PERIOD_MS;
    uint32_t phase = t % period;
    uint32_t tri = phase < period / 2 ? phase * 2 * FULL / period : 2 * FULL - phase * 2 * FULL / period;

    return 150 + 850 * tri / FULL * tri / FULL;
}

/*
 * Fuel gauge over all LEDs: each LED is two steps (half, full), so 3 LEDs give
 * 6 steps. 84-100 % lights all three, 1-16 % half of the first.
 */
static void render_gauge(int64_t t, bool breathing, bool blinking) {
    uint8_t level = 0;
#if HAS_BATTERY
    level = zmk_battery_state_of_charge();
#endif
    int steps = MIN(level * 2 * NUM_LEDS / 100 + 1, 2 * NUM_LEDS);
    int top = (steps - 1) / 2;
    struct color c;

    if (level >= CONFIG_LEDSTRIP_STATUS_LEVEL_GREEN) {
        c = GREEN;
    } else if (level >= CONFIG_LEDSTRIP_STATUS_LEVEL_YELLOW) {
        c = YELLOW;
    } else if (level >= CONFIG_LEDSTRIP_STATUS_LEVEL_AMBER) {
        c = AMBER;
    } else {
        c = RED;
        blinking = !breathing;
    }

    clear_leds();
    if (blinking && blink_off(t)) {
        return;
    }
    for (int i = 0; i <= top; i++) {
        uint32_t b = steps >= 2 * (i + 1) ? FULL : HALF;

        if (breathing && i == top) {
            b = b * breathe(t) / FULL;
        }
        set_led(i, c, b);
    }
}

static void render_profile(int64_t t) {
    clear_leds();
#if IS_CENTRAL
    if (zmk_endpoint_get_selected().transport == ZMK_TRANSPORT_USB) {
        for (int i = 0; i < NUM_LEDS; i++) {
            set_led(i, CYAN, FULL);
        }
        return;
    }
#if IS_ENABLED(CONFIG_ZMK_BLE)
    int profile = zmk_ble_active_profile_index();
    struct color c = zmk_ble_active_profile_is_connected() ? BLUE
                     : zmk_ble_active_profile_is_open()    ? AMBER
                                                           : RED;

    for (int i = 0; i < NUM_LEDS; i++) {
        set_led(i, WHITE, DIM);
    }
    // profiles past the last LED wrap around and blink
    if (profile < NUM_LEDS || !blink_off(t)) {
        set_led(profile % NUM_LEDS, c, FULL);
    }
#endif
#endif
}

static void render_link(void) {
    bool connected = false;
#if IS_BLE_PERIPHERAL
    connected = zmk_split_bt_peripheral_is_connected();
#endif

    for (int i = 0; i < NUM_LEDS; i++) {
        set_led(i, connected ? BLUE : RED, FULL);
    }
}

static void render(enum display display, int64_t t) {
    switch (display) {
    case DISPLAY_GAUGE:
        render_gauge(t, false, false);
        break;
    case DISPLAY_CHARGING:
        render_gauge(t, true, false);
        break;
    case DISPLAY_CRITICAL:
        render_gauge(t, false, true);
        break;
    case DISPLAY_CHARGED:
        for (int i = 0; i < NUM_LEDS; i++) {
            set_led(i, GREEN, FULL);
        }
        break;
    case DISPLAY_PROFILE:
        render_profile(t);
        break;
    case DISPLAY_LINK:
        render_link();
        break;
    }
}

/* ----- output ----- */

// switch the supply on if needed; false if the keyboard is going to sleep
static bool power_on(void) {
    bool switched = false;
    k_spinlock_key_t key = k_spin_lock(&power_lock);

    if (suspended) {
        k_spin_unlock(&power_lock, key);
        return false;
    }
    if (!powered) {
#if HAS_ENABLE
        gpio_pin_set_dt(&enable_gpio, 1);
#endif
        powered = true;
        switched = true;
    }
    k_spin_unlock(&power_lock, key);

    if (switched) {
        k_msleep(DT_PROP(STATUS_NODE, power_on_delay_ms));
    }
    return true;
}

static void power_off(void) {
    if (!powered) {
        return;
    }
    // blank first, so the LEDs are dark and the data line is left low
    clear_leds();
    led_strip_update_rgb(strip, pixels, NUM_LEDS);

    k_spinlock_key_t key = k_spin_lock(&power_lock);
#if HAS_ENABLE
    gpio_pin_set_dt(&enable_gpio, 0);
#endif
    powered = false;
    k_spin_unlock(&power_lock, key);
}

static bool show(void) {
    if (!power_on()) {
        return false;
    }
    int err = led_strip_update_rgb(strip, pixels, NUM_LEDS);
    if (err < 0) {
        LOG_ERR("Status LED update failed (%d)", err);
    }
    return true;
}

static void run(struct request req) {
    int64_t start = k_uptime_get();

    LOG_DBG("Status LEDs: display %d for %u ms", req.display, req.duration_ms);
    for (;;) {
        int64_t t = k_uptime_get() - start;
        struct request next;

        if (t >= req.duration_ms) {
            return;
        }
        render(req.display, t);
        if (!show()) {
            return;
        }
        k_msleep(MIN(CONFIG_LEDSTRIP_STATUS_FRAME_MS, req.duration_ms - t));

        // the same display requested again: restart its timeout
        if (k_msgq_peek(&request_q, &next) == 0 && next.display == req.display) {
            k_msgq_get(&request_q, &req, K_NO_WAIT);
            start = k_uptime_get();
        }
    }
}

static void status_thread(void *p1, void *p2, void *p3) {
    struct request req;

    for (;;) {
        k_msgq_get(&request_q, &req, K_FOREVER);
        run(req);

        if (k_msgq_num_used_get(&request_q) == 0) {
            power_off();
        } else if (powered) {
            clear_leds();
            show();
            k_msleep(CONFIG_LEDSTRIP_STATUS_GAP_MS);
        }
    }
}

K_THREAD_DEFINE(ledstrip_status_tid, CONFIG_LEDSTRIP_STATUS_THREAD_STACK_SIZE, status_thread, NULL,
                NULL, NULL, CONFIG_LEDSTRIP_STATUS_THREAD_PRIORITY, 0, 0);

/* ----- public API, used by the behaviors ----- */

void ledstrip_status_show_battery(void) {
    post(charging() ? DISPLAY_CHARGING : DISPLAY_GAUGE, CONFIG_LEDSTRIP_STATUS_SHOW_MS);
}

void ledstrip_status_show_connection(void) {
#if IS_CENTRAL
    post(DISPLAY_PROFILE, CONFIG_LEDSTRIP_STATUS_SHOW_MS);
#else
    post(DISPLAY_LINK, CONFIG_LEDSTRIP_STATUS_SHOW_MS);
#endif
}

/* ----- charger tracking ----- */

static bool was_usb_powered;
static bool was_charging;

static void charge_work_cb(struct k_work *work) {
    bool usb = usb_powered();
    bool chg = charging();

#if HAS_CHARGE
    // re-arm on the opposite level; level interrupts use the port SENSE
    // mechanism, which costs nothing in idle (edge interrupts would hold a
    // GPIOTE channel)
    gpio_pin_interrupt_configure_dt(&charge_gpio, gpio_pin_get_dt(&charge_gpio) > 0
                                                      ? GPIO_INT_LEVEL_INACTIVE
                                                      : GPIO_INT_LEVEL_ACTIVE);
#endif

    if (initialized) {
        if (HAS_CHARGE) {
            if (chg && !was_charging) {
                post(DISPLAY_CHARGING, CONFIG_LEDSTRIP_STATUS_CHARGING_SHOW_MS);
            } else if (!chg && usb && (was_charging || !was_usb_powered)) {
                // finished charging, or plugged in with a full battery
                post(DISPLAY_CHARGED, CONFIG_LEDSTRIP_STATUS_CHARGED_SHOW_MS);
            }
        } else if (usb && !was_usb_powered) {
            post(DISPLAY_CHARGING, CONFIG_LEDSTRIP_STATUS_CHARGING_SHOW_MS);
        }
    }
    was_usb_powered = usb;
    was_charging = chg;
}

// debounce: the charger settles a moment after USB is plugged or unplugged
static K_WORK_DELAYABLE_DEFINE(charge_work, charge_work_cb);
#define CHARGE_SETTLE K_MSEC(500)

#if HAS_CHARGE
static void charge_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins) {
    gpio_pin_interrupt_configure_dt(&charge_gpio, GPIO_INT_DISABLE);
    k_work_reschedule(&charge_work, CHARGE_SETTLE);
}
#endif

/* ----- boot ----- */

static void boot_work_cb(struct k_work *work) {
    charge_work_cb(NULL); // take the charger state without announcing it
    initialized = true;

    if (!IS_ENABLED(CONFIG_LEDSTRIP_STATUS_SHOW_ON_BOOT)) {
        return;
    }
    ledstrip_status_show_battery();
#if IS_CENTRAL && IS_ENABLED(CONFIG_ZMK_BLE)
    post(DISPLAY_PROFILE, CONFIG_LEDSTRIP_STATUS_SHOW_MS);
#elif IS_BLE_PERIPHERAL
    if (zmk_split_bt_peripheral_is_connected()) {
        post(DISPLAY_LINK, CONFIG_LEDSTRIP_STATUS_LINK_BLINK_MS);
    }
#endif
}

static K_WORK_DELAYABLE_DEFINE(boot_work, boot_work_cb);

static int ledstrip_status_init(void) {
    if (!device_is_ready(strip)) {
        LOG_ERR("Status LED strip %s is not ready", strip->name);
        return -ENODEV;
    }
#if HAS_ENABLE
    if (!gpio_is_ready_dt(&enable_gpio)) {
        return -ENODEV;
    }
    gpio_pin_configure_dt(&enable_gpio, GPIO_OUTPUT_INACTIVE);
#endif
#if HAS_CHARGE
    if (!gpio_is_ready_dt(&charge_gpio)) {
        return -ENODEV;
    }
    gpio_pin_configure_dt(&charge_gpio, GPIO_INPUT);
    gpio_init_callback(&charge_cb, charge_isr, BIT(charge_gpio.pin));
    gpio_add_callback_dt(&charge_gpio, &charge_cb);
#endif

    k_work_schedule(&boot_work, K_MSEC(CONFIG_LEDSTRIP_STATUS_BOOT_DELAY_MS));
    return 0;
}

SYS_INIT(ledstrip_status_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* ----- events ----- */

static int ledstrip_status_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *activity = as_zmk_activity_state_changed(eh);
    if (activity) {
        // handled synchronously: the system powers off right after this event
        k_spinlock_key_t key = k_spin_lock(&power_lock);
        suspended = activity->state == ZMK_ACTIVITY_SLEEP;
        if (suspended) {
#if HAS_ENABLE
            gpio_pin_set_dt(&enable_gpio, 0);
#endif
            powered = false;
        }
        k_spin_unlock(&power_lock, key);
        if (suspended) {
            k_msgq_purge(&request_q);
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (!initialized) {
        return ZMK_EV_EVENT_BUBBLE;
    }

#if HAS_BATTERY
    const struct zmk_battery_state_changed *battery = as_zmk_battery_state_changed(eh);
    if (battery) {
        if (battery->state_of_charge < CONFIG_LEDSTRIP_STATUS_LEVEL_CRITICAL && !usb_powered()) {
            post(DISPLAY_CRITICAL, CONFIG_LEDSTRIP_STATUS_CRITICAL_SHOW_MS);
        }
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_USB)
    if (as_zmk_usb_conn_state_changed(eh)) {
        k_work_reschedule(&charge_work, CHARGE_SETTLE);
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

    if (IS_ENABLED(CONFIG_LEDSTRIP_STATUS_SHOW_CONN_CHANGES)) {
#if IS_CENTRAL
        post(DISPLAY_PROFILE, CONFIG_LEDSTRIP_STATUS_SHOW_MS);
#else
        post(DISPLAY_LINK, CONFIG_LEDSTRIP_STATUS_LINK_BLINK_MS);
#endif
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(ledstrip_status, ledstrip_status_listener);
ZMK_SUBSCRIPTION(ledstrip_status, zmk_activity_state_changed);
#if HAS_BATTERY
ZMK_SUBSCRIPTION(ledstrip_status, zmk_battery_state_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_USB)
ZMK_SUBSCRIPTION(ledstrip_status, zmk_usb_conn_state_changed);
#endif
#if IS_CENTRAL && IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(ledstrip_status, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(ledstrip_status, zmk_endpoint_changed);
#endif
#if IS_BLE_PERIPHERAL
ZMK_SUBSCRIPTION(ledstrip_status, zmk_split_peripheral_status_changed);
#endif
