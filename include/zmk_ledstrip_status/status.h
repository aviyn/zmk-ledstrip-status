#pragma once

/* Show this half's battery gauge (animated while charging). */
void ledstrip_status_show_battery(void);

/* Central: show the active BLE profile and its state. Peripheral: show the split link state. */
void ledstrip_status_show_connection(void);
