/*
 * hotkeys.h — Global hotkey config and runtime helpers.
 */

#ifndef VARNISH_HOTKEYS_H
#define VARNISH_HOTKEYS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    VARNISH_HOTKEY_BUTTON_NONE = 0,
    VARNISH_HOTKEY_BUTTON_UP,
    VARNISH_HOTKEY_BUTTON_DOWN,
    VARNISH_HOTKEY_BUTTON_LEFT,
    VARNISH_HOTKEY_BUTTON_RIGHT,
    VARNISH_HOTKEY_BUTTON_A,
    VARNISH_HOTKEY_BUTTON_B,
    VARNISH_HOTKEY_BUTTON_X,
    VARNISH_HOTKEY_BUTTON_Y,
    VARNISH_HOTKEY_BUTTON_L1,
    VARNISH_HOTKEY_BUTTON_L2,
    VARNISH_HOTKEY_BUTTON_R1,
    VARNISH_HOTKEY_BUTTON_R2,
    VARNISH_HOTKEY_BUTTON_START,
    VARNISH_HOTKEY_BUTTON_SELECT,
    VARNISH_HOTKEY_BUTTON_MENU,
    VARNISH_HOTKEY_BUTTON_COUNT
} varnish_hotkey_button;

typedef enum {
    VARNISH_HOTKEY_ACTION_NONE = 0,
    VARNISH_HOTKEY_ACTION_SCREENSHOT
} varnish_hotkey_action;

typedef struct {
    uint32_t screenshot_mask;
} varnish_hotkey_config;

typedef struct {
    uint32_t screenshot_mask;
    bool paused;
    bool screenshot_latched;
    bool wait_for_release;
} varnish_hotkey_logic;

const char *hotkeys_button_name(varnish_hotkey_button button);
bool hotkeys_button_supported(varnish_hotkey_button button);
uint32_t hotkeys_button_bit(varnish_hotkey_button button);
int hotkeys_mask_button_count(uint32_t mask);

int hotkeys_parse_mask(const char *text, uint32_t *out_mask);
int hotkeys_format_mask(uint32_t mask, char *out, size_t out_size);

void hotkeys_config_init(varnish_hotkey_config *config);
bool hotkeys_config_has_bindings(const varnish_hotkey_config *config);
int hotkeys_load_config(varnish_hotkey_config *config);
int hotkeys_save_config(const varnish_hotkey_config *config);

void hotkeys_logic_init(varnish_hotkey_logic *logic, uint32_t screenshot_mask);
void hotkeys_logic_set_binding(varnish_hotkey_logic *logic, uint32_t screenshot_mask);
void hotkeys_logic_set_paused(varnish_hotkey_logic *logic, bool paused);
varnish_hotkey_action hotkeys_logic_update(varnish_hotkey_logic *logic,
                                           uint32_t pressed_mask);

int hotkeys_runtime_init(void);
void hotkeys_runtime_cleanup(void);
int hotkeys_runtime_reload(void);
void hotkeys_runtime_set_paused(bool paused);
varnish_hotkey_action hotkeys_runtime_poll(void);

#endif /* VARNISH_HOTKEYS_H */
