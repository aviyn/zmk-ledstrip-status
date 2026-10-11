# zmk-ledstrip-status

A ZMK module that shows battery, charging and connection status on a short chain of addressable LEDs (WS2812 / SK6812). It's built for the 3-LED status bar on each half of the Forager Choc v2 board, but works with any strip length.

It's modelled on [zmk-rgbled-widget](https://github.com/caksoylar/zmk-rgbled-widget), which only drives a plain 3-GPIO RGB LED. Do not enable `CONFIG_ZMK_RGB_UNDERGLOW` on the same strip.

## What it shows

| When | Display |
|---|---|
| Boot | Battery gauge for 3 s. The central then shows its BLE profile; the peripheral blinks blue if it's linked. |
| BLE profile change, connect or disconnect (central) | Profile view for 3 s: one LED per profile, the active one blue (connected), amber (advertising, unpaired) or red (paired, not connected), the others dim white. Profiles past the last LED wrap around and blink. When output is USB, all LEDs are cyan. |
| Split link change (peripheral) | One blink: blue when connected, red when disconnected. |
| Charging starts | Battery gauge with the highest lit LED breathing, for 5 s. |
| Charging completes, or USB plugged into a full battery | Solid green for 3 s. |
| Battery level change below 10 % | The gauge blinks red. |
| Battery reaches a new lowest gauge step (`SHOW_ON_LEVEL_DROP`) | Battery gauge for 3 s. Going back up doesn't count; on USB the step follows the level. |
| Sleep | Off, and the supply is cut. |

### Persistent indicators

These stay lit while no timed display runs, and come back after one ends:

| Indicator | LED | |
|---|---|---|
| Layer | `layer-led` (default 0) | The highest active layer's color from `layer-colors`. A 0 entry shows nothing. A held layer (`&mo`, `&lt`, conditional) shows while held. A toggled one (`&tog`, `&to`) only flashes for `LAYER_FLASH_MS` (1 s). Central only. |
| Caps word | `caps-led` (default: last LED) | `caps-word-color`, default magenta. Central only. |
| Caps lock | `caps-led` | `caps-lock-color`, default white. Shown on every half, through ZMK's split HID-indicator relay. |
| Charging (`CHARGING_ALWAYS`) | all | The charging animation for as long as the battery charges. Layer and caps indicators show on top. |

- **Idle:** while idle on battery, the layer and caps indicators hide (`INDICATORS_IDLE_OFF`), and any key brings them back.
- **Power:** each lit indicator keeps the strip powered, about 1 mA per LED plus the LED itself.
- **Caps word** has no ZMK event or public state. The module reads the `&caps_word` device's internal `active` flag shortly after each key event.

### Battery gauge

Each LED is two steps (half, then full), so 3 LEDs give 6 steps. The gauge fills from logical LED 0:

| Battery | LEDs | Colour |
|---|---|---|
| 84–100 % | ● ● ● | green |
| 67–83 % | ● ● ◐ | green |
| 50–66 % | ● ● ○ | green |
| 34–49 % | ● ◐ ○ | yellow |
| 17–33 % | ● ○ ○ | amber |
| 0–16 % | ◐ ○ ○ | red, blinking |

## Power

The strip's supply is switched on only while something is shown. Use `enable-gpios`, for example the gate of a P-FET high-side switch. Each SK6812 draws about 1 mA even when dark. Before cutting the supply, the module blanks the strip, so the data line is left low and can't back-power the LEDs.

## Usage

Add the module to `config/west.yml`:

```yaml
  remotes:
    - name: aviyn
      url-base: https://github.com/aviyn
  projects:
    - name: zmk-ledstrip-status
      remote: aviyn
      revision: main
```

Describe the strip and the status node in the shield's devicetree. The module turns on by itself when a `zmk,ledstrip-status` node exists.

```dts
#include <dt-bindings/led/led.h>

&spi3 {
    compatible = "nordic,nrf-spim";
    status = "okay";
    pinctrl-0 = <&led_spi_default>;
    pinctrl-1 = <&led_spi_sleep>;
    pinctrl-names = "default", "sleep";

    led_strip: ws2812@0 {
        compatible = "worldsemi,ws2812-spi";
        reg = <0>;
        spi-max-frequency = <4000000>;
        chain-length = <3>;
        spi-one-frame = <0x70>;
        spi-zero-frame = <0x40>;
        reset-delay = <80>;  /* SK6812 */
        color-mapping = <LED_COLOR_ID_GREEN LED_COLOR_ID_RED LED_COLOR_ID_BLUE>;
    };
};

/ {
    status_leds {
        compatible = "zmk,ledstrip-status";
        led-strip = <&led_strip>;
        enable-gpios = <&gpio1 15 GPIO_ACTIVE_LOW>;                    /* optional */
        charge-gpios = <&gpio0 17 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>;   /* optional, XIAO nRF52840 CHG */
        led-order = <2 1 0>;                                           /* optional */
        layer-colors = <0 0x0000ff 0x00ffff>;                          /* optional */
    };
};
```

Without `charge-gpios`, plugging in USB shows the charging animation, but completion can't be detected.

### Keymap behaviors

```dts
#include <behaviors/ledstrip_status.dtsi>

&led_status_bat   // show the battery gauge
&led_status_con   // show the profile (central) or split link (peripheral)
```

Both have global locality: one press on either half makes every half show its own status.

## Configuration

All options are in [`Kconfig`](Kconfig). The main ones:

| Option | Default | |
|---|---|---|
| `CONFIG_LEDSTRIP_STATUS_BRIGHTNESS` | 32 | full brightness per channel, 0–255 |
| `CONFIG_LEDSTRIP_STATUS_HALF_PERCENT` | 40 | half-lit gauge LED |
| `CONFIG_LEDSTRIP_STATUS_SHOW_MS` | 3000 | gauge and profile duration |
| `CONFIG_LEDSTRIP_STATUS_SHOW_ON_BOOT` | y | |
| `CONFIG_LEDSTRIP_STATUS_SHOW_CONN_CHANGES` | y | |
| `CONFIG_LEDSTRIP_STATUS_LEVEL_GREEN` / `_YELLOW` / `_AMBER` | 50 / 34 / 17 | gauge colour thresholds |
| `CONFIG_LEDSTRIP_STATUS_LEVEL_CRITICAL` | 10 | 0 disables the warning |
| `CONFIG_LEDSTRIP_STATUS_CHARGING_SHOW_MS` | 5000 | |
| `CONFIG_LEDSTRIP_STATUS_CHARGING_ALWAYS` | n | animate for as long as charging lasts |
| `CONFIG_LEDSTRIP_STATUS_SHOW_ON_LEVEL_DROP` | n | gauge on each new lowest step |
| `CONFIG_LEDSTRIP_STATUS_SHOW_LAYER` / `_CAPS_LOCK` / `_CAPS_WORD` | y | persistent indicators |
| `CONFIG_LEDSTRIP_STATUS_LAYER_FLASH_MS` | 1000 | toggled layers; 0 = lit for as long as on |
| `CONFIG_LEDSTRIP_STATUS_INDICATOR_PERCENT` | 100 | indicator brightness |
| `CONFIG_LEDSTRIP_STATUS_INDICATORS_IDLE_OFF` | y | |

## Tests

[`tests/`](tests) runs the module on ZMK's `native_sim` test board in GitHub Actions ([`.github/workflows/test.yml`](.github/workflows/test.yml)). It uses ZMK's own `run-test.sh` and its mock key matrix.

- **Test doubles** in [`src/test/`](src/test/test_support.c), built only with `CONFIG_LEDSTRIP_STATUS_TEST`:
  - a fake LED strip that logs every changed frame, with a timestamp and whether the supply switch is on
  - a fuel gauge whose level the test sets
  - a test behavior, `&lst`, whose keys set the battery level, VBUS and the charger input on emulated GPIOs, set the host's caps lock, or send sleep and wake
- **Cases:**
  - `boot`, `boot-order`: the boot gauge, plus the reversed `led-order`
  - `levels`: every gauge step, the red blink, and the critical warning
  - `charging`: settle, breathing, the key while charging, completion, unplugging, and plugging in with a full battery
  - `sleep`: the supply is cut at once, and requests while asleep are ignored
  - `queue`: restart on a repeat press, then the next display after a dark gap
  - `indicators`: the layer color, caps lock, caps word over caps lock, return after a timed display, and hiding on ZMK's real idle
  - `charging-always`: the animation for the whole charge, with the layer color on top
  - `level-drop`: only new lowest steps are shown, including after charging
- **Snapshots:** each case compares its log with `keycode_events.snapshot`. After an intended change, run the workflow manually with **accept** ticked, review the new snapshots in the `test-logs` artifact, and commit them.

Not covered: the BLE profile view and the split link, because `native_sim` has no BLE; real LED timing; and current draw.

Targets ZMK `main` (Zephyr 4.1).
