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
#include <zmk/keymap.h>
#include <dt-bindings/zmk/hid_indicators.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
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
#define HAS_VBUS DT_NODE_HAS_PROP(STATUS_NODE, vbus_gpios)

static const struct device *strip = DEVICE_DT_GET(STRIP_NODE);

#if HAS_ENABLE
static const struct gpio_dt_spec enable_gpio = GPIO_DT_SPEC_GET(STATUS_NODE, enable_gpios);
#endif
#if HAS_CHARGE
static const struct gpio_dt_spec charge_gpio = GPIO_DT_SPEC_GET(STATUS_NODE, charge_gpios);
static struct gpio_callback charge_cb;
#endif
#if HAS_VBUS
static const struct gpio_dt_spec vbus_gpio = GPIO_DT_SPEC_GET(STATUS_NODE, vbus_gpios);
static struct gpio_callback vbus_cb;
#endif

#if DT_NODE_HAS_PROP(STATUS_NODE, led_order)
static const uint8_t led_order[] = DT_PROP(STATUS_NODE, led_order);
BUILD_ASSERT(ARRAY_SIZE(led_order) == NUM_LEDS, "led-order must list every LED of the strip");
#define STRIP_INDEX(i) (led_order[i])
#else
#define STRIP_INDEX(i) (i)
#endif

#define SHOW_LAYER                                                                                 \
    (IS_ENABLED(CONFIG_LEDSTRIP_STATUS_SHOW_LAYER) && IS_CENTRAL &&                                \
     DT_NODE_HAS_PROP(STATUS_NODE, layer_colors))
#define SHOW_CAPS_LOCK                                                                             \
    (IS_ENABLED(CONFIG_LEDSTRIP_STATUS_SHOW_CAPS_LOCK) && IS_ENABLED(CONFIG_ZMK_HID_INDICATORS))
// caps word has no event or public state; its device data is a single `bool active`
#define SHOW_CAPS_WORD                                                                             \
    (IS_ENABLED(CONFIG_LEDSTRIP_STATUS_SHOW_CAPS_WORD) && IS_CENTRAL &&                            \
     DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(caps_word)))

#define LAYER_LED DT_PROP(STATUS_NODE, layer_led)
#define CAPS_LED DT_PROP_OR(STATUS_NODE, caps_led, NUM_LEDS - 1)
BUILD_ASSERT(LAYER_LED < NUM_LEDS && CAPS_LED < NUM_LEDS,
             "layer-led and caps-led must be LEDs of the strip");

#if SHOW_LAYER
static const uint32_t layer_colors[] = DT_PROP(STATUS_NODE, layer_colors);
#endif
#if SHOW_CAPS_WORD
static const struct device *caps_word_dev = DEVICE_DT_GET(DT_NODELABEL(caps_word));
#endif

#define FULL 1000 // brightness scale, in permille of CONFIG_LEDSTRIP_STATUS_BRIGHTNESS
#define HALF (CONFIG_LEDSTRIP_STATUS_HALF_PERCENT * 10)
#define DIM (CONFIG_LEDSTRIP_STATUS_DIM_PERCENT * 10)
#define INDICATOR (CONFIG_LEDSTRIP_STATUS_INDICATOR_PERCENT * 10)

struct color {
    uint8_t r, g, b;
};

static const struct color RED __maybe_unused = {255, 0, 0};
static const struct color AMBER __maybe_unused = {255, 70, 0};
static const struct color YELLOW __maybe_unused = {255, 170, 0};
static const struct color GREEN __maybe_unused = {0, 255, 0};
static const struct color BLUE __maybe_unused = {0, 0, 255};
static const struct color CYAN __maybe_unused = {0, 200, 255};
static const struct color WHITE __maybe_unused = {255, 255, 255};

static struct color __maybe_unused hex_color(uint32_t rgb) {
    return (struct color){rgb >> 16, (rgb >> 8) & 0xff, rgb & 0xff};
}

enum display {
    DISPLAY_GAUGE,    // battery gauge
    DISPLAY_CHARGING, // battery gauge, highest lit LED breathing
    DISPLAY_CHARGED,  // all LEDs green
    DISPLAY_CRITICAL, // battery gauge, blinking
    DISPLAY_PROFILE,  // central: active BLE profile and its state
    DISPLAY_LINK,     // peripheral: split link connected (blue) or not (red)
    DISPLAY_REFRESH,  // not a display: wakes the thread to redraw the background
};

static const char *const display_names[] = {"gauge",   "charging", "charged", "critical",
                                            "profile", "link",     "refresh"};

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

// background state, written by the event side and read by the display thread
static bool idle;
static bool bg_charging;
static zmk_hid_indicators_t hid_indicators;
static atomic_t refresh_pending;

// the last frame sent had a lit LED
static bool lit;

static void post(enum display display, uint32_t duration_ms) {
    struct request req = {.display = display, .duration_ms = duration_ms};

    if (suspended) {
        return;
    }
    if (k_msgq_put(&request_q, &req, K_NO_WAIT) < 0) {
        LOG_WRN("Status LED queue full, dropping display %d", display);
    }
}

// redraw the background, e.g. after a layer or caps change
static void refresh(void) {
    struct request req = {.display = DISPLAY_REFRESH};

    if (!atomic_cas(&refresh_pending, 0, 1)) {
        return;
    }
    if (suspended || k_msgq_put(&request_q, &req, K_NO_WAIT) < 0) {
        atomic_clear(&refresh_pending);
    }
}

/* ----- charger state ----- */

static bool usb_powered(void) {
#if HAS_VBUS
    return gpio_pin_get_dt(&vbus_gpio) > 0;
#elif defined(NRF_POWER_HAS_USBREG) && NRF_POWER_HAS_USBREG
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
static int gauge_steps(uint8_t level) { return MIN(level * 2 * NUM_LEDS / 100 + 1, 2 * NUM_LEDS); }

static void render_gauge(int64_t t, bool breathing, bool blinking) {
    uint8_t level = 0;
#if HAS_BATTERY
    level = zmk_battery_state_of_charge();
#endif
    int steps = gauge_steps(level);
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
    case DISPLAY_REFRESH:
        break;
    }
}

/*
 * What the strip shows when no timed display runs: the charging animation
 * (CHARGING_ALWAYS), with the layer color and caps state on their own LEDs on
 * top. Returns whether anything is lit; sets *animated if it needs frames.
 */
static bool render_background(int64_t t, bool *animated) {
    bool any = false;

    clear_leds();
    *animated = false;
    if (suspended) {
        return false;
    }
    if (bg_charging) {
        render_gauge(t, true, false);
        *animated = true;
        any = true;
    }
    if (IS_ENABLED(CONFIG_LEDSTRIP_STATUS_INDICATORS_IDLE_OFF) && idle && !usb_powered()) {
        return any;
    }
#if SHOW_LAYER
    uint8_t layer = zmk_keymap_highest_layer_active();
    if (layer < ARRAY_SIZE(layer_colors) && layer_colors[layer] != 0) {
        set_led(LAYER_LED, hex_color(layer_colors[layer]), INDICATOR);
        any = true;
    }
#endif
#if SHOW_CAPS_WORD
    if (*(const bool *)caps_word_dev->data) {
        set_led(CAPS_LED, hex_color(DT_PROP(STATUS_NODE, caps_word_color)), INDICATOR);
        return true;
    }
#endif
#if SHOW_CAPS_LOCK
    if (hid_indicators & HID_INDICATOR_CAPS_LOCK) {
        set_led(CAPS_LED, hex_color(DT_PROP(STATUS_NODE, caps_lock_color)), INDICATOR);
        any = true;
    }
#endif
    return any;
}

/* ----- output ----- */

// switch the supply on if needed; false if the keyboard is going to sleep
static bool status_power_on(void) {
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
        LOG_DBG("supply on");
        k_msleep(DT_PROP(STATUS_NODE, power_on_delay_ms));
    }
    return true;
}

static void status_power_off(void) {
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
    LOG_DBG("supply off");
}

static bool show(void) {
    if (!status_power_on()) {
        return false;
    }
    lit = false;
    for (int i = 0; i < NUM_LEDS; i++) {
        lit |= pixels[i].r || pixels[i].g || pixels[i].b;
    }
    int err = led_strip_update_rgb(strip, pixels, NUM_LEDS);
    if (err < 0) {
        LOG_ERR("Status LED update failed (%d)", err);
    }
    return true;
}

// a short dark pause between two different things on the strip
static void gap(void) {
    if (powered && lit) {
        clear_leds();
        show();
        k_msleep(CONFIG_LEDSTRIP_STATUS_GAP_MS);
    }
}

static void status_run(struct request req) {
    int64_t start = k_uptime_get();

    LOG_DBG("show %s for %u ms", display_names[req.display], req.duration_ms);
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
            LOG_DBG("restart %s for %u ms", display_names[req.display], req.duration_ms);
        }
    }
}

static void status_thread(void *p1, void *p2, void *p3) {
    struct request req;
    bool animated;

    for (;;) {
        // between timed displays: the background, or dark with the supply off
        bool background = render_background(k_uptime_get(), &animated);
        if (background) {
            show();
        } else {
            status_power_off();
        }

        // fails on timeout (next background frame), and with -ENOMSG when the
        // queue is purged for sleep
        k_timeout_t wait =
            background && animated ? K_MSEC(CONFIG_LEDSTRIP_STATUS_FRAME_MS) : K_FOREVER;
        if (k_msgq_get(&request_q, &req, wait) < 0) {
            continue;
        }
        bool ran = false;
        do {
            if (req.display == DISPLAY_REFRESH) {
                atomic_clear(&refresh_pending);
                continue;
            }
            gap();
            status_run(req);
            ran = true;
        } while (k_msgq_get(&request_q, &req, K_NO_WAIT) == 0);

        // back to the background after a timed display; a background change
        // alone (layer, caps) switches without a gap
        if (ran && render_background(k_uptime_get(), &animated)) {
            gap();
        }
    }
}

K_THREAD_DEFINE(ledstrip_status_tid, CONFIG_LEDSTRIP_STATUS_THREAD_STACK_SIZE, status_thread, NULL,
                NULL, NULL, CONFIG_LEDSTRIP_STATUS_THREAD_PRIORITY, 0, 0);

/* ----- public API, used by the behaviors ----- */

void ledstrip_status_show_battery(void) {
    if (bg_charging) {
        return; // the charging animation is already showing
    }
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

// re-arm on the opposite level; level interrupts use the port SENSE mechanism,
// which costs nothing in idle (edge interrupts would hold a GPIOTE channel)
static void __maybe_unused rearm(const struct gpio_dt_spec *spec) {
    gpio_pin_interrupt_configure_dt(spec, gpio_pin_get_dt(spec) > 0 ? GPIO_INT_LEVEL_INACTIVE
                                                                    : GPIO_INT_LEVEL_ACTIVE);
}

static void charge_work_cb(struct k_work *work) {
    bool usb = usb_powered();
    bool chg = charging();

#if HAS_CHARGE
    rearm(&charge_gpio);
#endif
#if HAS_VBUS
    rearm(&vbus_gpio);
#endif

    bool always = IS_ENABLED(CONFIG_LEDSTRIP_STATUS_CHARGING_ALWAYS) && chg;
    if (always != bg_charging) {
        bg_charging = always;
        refresh();
    }

    if (initialized) {
        if (HAS_CHARGE) {
            if (chg && !was_charging) {
                if (!always) {
                    post(DISPLAY_CHARGING, CONFIG_LEDSTRIP_STATUS_CHARGING_SHOW_MS);
                }
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
#if HAS_VBUS
static void vbus_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins) {
    gpio_pin_interrupt_configure_dt(&vbus_gpio, GPIO_INT_DISABLE);
    k_work_reschedule(&charge_work, CHARGE_SETTLE);
}
#endif

/* ----- caps word, which has no event: check shortly after each key ----- */

#if SHOW_CAPS_WORD
static bool was_caps_word;

static void caps_word_work_cb(struct k_work *work) {
    bool active = *(const bool *)caps_word_dev->data;

    if (active != was_caps_word) {
        was_caps_word = active;
        refresh();
    }
}

static K_WORK_DELAYABLE_DEFINE(caps_word_work, caps_word_work_cb);
#endif

/* ----- battery level drops ----- */

#define NO_STEP 0xff
static uint8_t lowest_step = NO_STEP;

// true each time the gauge reaches a new lowest step on battery
static bool level_dropped(uint8_t level) {
    uint8_t step = gauge_steps(level);

    if (usb_powered()) {
        lowest_step = step; // charging: start over from the level at unplugging
        return false;
    }
    if (lowest_step == NO_STEP || step >= lowest_step) {
        lowest_step = MIN(step, lowest_step);
        return false;
    }
    lowest_step = step;
    return true;
}

/* ----- boot ----- */

static void boot_work_cb(struct k_work *work) {
    charge_work_cb(NULL); // take the charger state without announcing it
#if HAS_BATTERY
    level_dropped(zmk_battery_state_of_charge()); // baseline
#endif
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
#if HAS_VBUS
    if (!gpio_is_ready_dt(&vbus_gpio)) {
        return -ENODEV;
    }
    gpio_pin_configure_dt(&vbus_gpio, GPIO_INPUT);
    gpio_init_callback(&vbus_cb, vbus_isr, BIT(vbus_gpio.pin));
    gpio_add_callback_dt(&vbus_gpio, &vbus_cb);
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
            atomic_clear(&refresh_pending);
            LOG_DBG("sleep: supply off");
        } else {
            idle = activity->state == ZMK_ACTIVITY_IDLE;
            refresh();
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    // indicators are tracked from boot on
#if SHOW_CAPS_LOCK
    const struct zmk_hid_indicators_changed *ind = as_zmk_hid_indicators_changed(eh);
    if (ind) {
        hid_indicators = ind->indicators;
        refresh();
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif
#if SHOW_LAYER
    if (as_zmk_layer_state_changed(eh)) {
        refresh();
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif
#if SHOW_CAPS_WORD
    if (as_zmk_position_state_changed(eh) || as_zmk_keycode_state_changed(eh)) {
        k_work_reschedule(&caps_word_work, K_MSEC(5));
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

    if (!initialized) {
        return ZMK_EV_EVENT_BUBBLE;
    }

#if HAS_BATTERY
    const struct zmk_battery_state_changed *battery = as_zmk_battery_state_changed(eh);
    if (battery) {
        bool dropped = level_dropped(battery->state_of_charge);

        if (battery->state_of_charge < CONFIG_LEDSTRIP_STATUS_LEVEL_CRITICAL && !usb_powered()) {
            post(DISPLAY_CRITICAL, CONFIG_LEDSTRIP_STATUS_CRITICAL_SHOW_MS);
        } else if (dropped && IS_ENABLED(CONFIG_LEDSTRIP_STATUS_SHOW_ON_LEVEL_DROP)) {
            post(DISPLAY_GAUGE, CONFIG_LEDSTRIP_STATUS_SHOW_MS);
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
#if SHOW_CAPS_LOCK
ZMK_SUBSCRIPTION(ledstrip_status, zmk_hid_indicators_changed);
#endif
#if SHOW_LAYER
ZMK_SUBSCRIPTION(ledstrip_status, zmk_layer_state_changed);
#endif
#if SHOW_CAPS_WORD
ZMK_SUBSCRIPTION(ledstrip_status, zmk_position_state_changed);
ZMK_SUBSCRIPTION(ledstrip_status, zmk_keycode_state_changed);
#endif
