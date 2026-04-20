/*
 * hotkeys.c — Global hotkey config and runtime helpers.
 */

#include "hotkeys.h"

#include "device.h"
#include "strutil.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if defined(PLATFORM_TG5040) || defined(PLATFORM_TG5050)
#include <SDL2/SDL.h>
#endif

#if defined(PLATFORM_MY355) && defined(__linux__)
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#define HOTKEYS_MAX_PATH 512
#define HOTKEYS_CONFIG_NAME "keybinds.txt"
#define HOTKEYS_SCAN_INTERVAL_MS 2000u
#define HOTKEYS_TRIMUI_MAX_JOYSTICKS 8
#define HOTKEYS_AXIS_DEADZONE 16000

typedef struct {
#if defined(PLATFORM_TG5040) || defined(PLATFORM_TG5050)
    SDL_Joystick *joysticks[HOTKEYS_TRIMUI_MAX_JOYSTICKS];
    int joystick_count;
    bool joystick_ready;
    bool joystick_subsystem_owned;
    bool joystick_event_state_saved;
    int joystick_event_state;
#endif
#if defined(PLATFORM_MY355) && defined(__linux__)
    int raw_input_fd;
    uint32_t raw_pressed_mask;
    char raw_input_path[32];
#endif
    varnish_hotkey_config config;
    varnish_hotkey_logic logic;
    uint32_t next_scan_ms;
    bool initialized;
} hotkeys_runtime_state;

static hotkeys_runtime_state g_hotkeys;

static const varnish_hotkey_button hotkey_button_order[] = {
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
    VARNISH_HOTKEY_BUTTON_F1,
    VARNISH_HOTKEY_BUTTON_F2,
};

static const char *hotkey_button_names[VARNISH_HOTKEY_BUTTON_COUNT] = {
    "NONE",
    "UP",
    "DOWN",
    "LEFT",
    "RIGHT",
    "A",
    "B",
    "X",
    "Y",
    "L1",
    "L2",
    "R1",
    "R2",
    "START",
    "SELECT",
    "MENU",
    "F1",
    "F2",
};

static void hotkeys_log_parse_error(const char *detail) {
    fprintf(stderr, "varnish: hotkeys: %s\n", detail);
}

#if defined(PLATFORM_TG5040) || defined(PLATFORM_TG5050) || \
    (defined(PLATFORM_MY355) && defined(__linux__))
static uint32_t hotkeys_now_ms(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0u;
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}
#endif

static char *trim_ascii(char *text) {
    char *end;

    if (!text) return text;
    while (*text && isspace((unsigned char)*text))
        text++;
    if (!*text) return text;

    end = text + strlen(text) - 1;
    while (end >= text && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return text;
}

static void hotkeys_mkdirp(const char *path) {
    char tmp[HOTKEYS_MAX_PATH];
    char *p;

    if (!path || !path[0]) return;
    str_copy_trunc(tmp, sizeof(tmp), path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static int hotkeys_get_state_dir(char *out, size_t out_size) {
    char userdata[HOTKEYS_MAX_PATH];

    device_get_userdata_path(userdata, sizeof(userdata));
    if (!userdata[0] || path_join(out, out_size, userdata, "Varnish") != 0) {
        if (out_size > 0) out[0] = '\0';
        return -1;
    }
    return 0;
}

static int hotkeys_get_config_path(char *out, size_t out_size) {
    char state_dir[HOTKEYS_MAX_PATH];

    if (hotkeys_get_state_dir(state_dir, sizeof(state_dir)) != 0 ||
        path_join(out, out_size, state_dir, HOTKEYS_CONFIG_NAME) != 0) {
        if (out_size > 0) out[0] = '\0';
        return -1;
    }
    return 0;
}

const char *hotkeys_button_name(varnish_hotkey_button button) {
    if (button <= VARNISH_HOTKEY_BUTTON_NONE || button >= VARNISH_HOTKEY_BUTTON_COUNT)
        return "NONE";
    return hotkey_button_names[button];
}

bool hotkeys_button_supported(varnish_hotkey_button button) {
    return button > VARNISH_HOTKEY_BUTTON_NONE &&
           button < VARNISH_HOTKEY_BUTTON_COUNT;
}

uint32_t hotkeys_button_bit(varnish_hotkey_button button) {
    if (!hotkeys_button_supported(button))
        return 0u;
    return 1u << (button - 1);
}

int hotkeys_mask_button_count(uint32_t mask) {
    int count = 0;

    while (mask) {
        count += (mask & 1u) ? 1 : 0;
        mask >>= 1u;
    }
    return count;
}

static int hotkeys_button_from_token(const char *token,
                                     varnish_hotkey_button *out_button) {
    char upper[24];
    size_t len;

    if (!token || !token[0] || !out_button)
        return -1;

    len = strlen(token);
    if (len >= sizeof(upper))
        return -1;

    for (size_t i = 0; i < len; i++)
        upper[i] = (char)toupper((unsigned char)token[i]);
    upper[len] = '\0';

    for (int i = 1; i < VARNISH_HOTKEY_BUTTON_COUNT; i++) {
        if (strcmp(upper, hotkey_button_names[i]) == 0) {
            *out_button = (varnish_hotkey_button)i;
            return 0;
        }
    }

    return -1;
}

int hotkeys_parse_mask(const char *text, uint32_t *out_mask) {
    char buf[128];
    char *save = NULL;
    char *part;
    uint32_t mask = 0;

    if (!text || !text[0] || !out_mask)
        return -1;
    if (strlen(text) >= sizeof(buf))
        return -1;

    str_copy_trunc(buf, sizeof(buf), text);
    for (part = strtok_r(buf, "+", &save); part;
         part = strtok_r(NULL, "+", &save)) {
        varnish_hotkey_button button;
        uint32_t bit;
        char *token = trim_ascii(part);

        if (!token[0])
            return -1;
        if (hotkeys_button_from_token(token, &button) != 0)
            return -1;

        bit = hotkeys_button_bit(button);
        if ((mask & bit) != 0u)
            return -1;
        mask |= bit;
    }

    if (hotkeys_mask_button_count(mask) < 2 || hotkeys_mask_button_count(mask) > 4)
        return -1;

    *out_mask = mask;
    return 0;
}

int hotkeys_format_mask(uint32_t mask, char *out, size_t out_size) {
    bool first = true;

    if (!out || out_size == 0)
        return -1;
    out[0] = '\0';

    if (mask == 0u)
        return 0;

    for (size_t i = 0; i < sizeof(hotkey_button_order) / sizeof(hotkey_button_order[0]); i++) {
        varnish_hotkey_button button = hotkey_button_order[i];
        uint32_t bit = hotkeys_button_bit(button);

        if ((mask & bit) == 0u)
            continue;

        if (!first && str_append(out, out_size, "+") != 0)
            return -1;
        if (str_append(out, out_size, hotkeys_button_name(button)) != 0)
            return -1;
        first = false;
    }

    return 0;
}

void hotkeys_config_init(varnish_hotkey_config *config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
}

bool hotkeys_config_has_bindings(const varnish_hotkey_config *config) {
    return config &&
           (config->screenshot_mask != 0u || config->manual_mask != 0u);
}

int hotkeys_load_config(varnish_hotkey_config *config) {
    char path[HOTKEYS_MAX_PATH];
    FILE *f;
    char line[256];
    int had_error = 0;

    hotkeys_config_init(config);
    if (!config)
        return -1;
    if (hotkeys_get_config_path(path, sizeof(path)) != 0)
        return -1;

    f = fopen(path, "rb");
    if (!f)
        return (errno == ENOENT) ? 0 : -1;

    while (fgets(line, sizeof(line), f)) {
        char *eq;
        char *key;
        char *value;

        key = trim_ascii(line);
        if (!key[0] || key[0] == '#')
            continue;

        eq = strchr(key, '=');
        if (!eq) {
            hotkeys_log_parse_error("invalid config line (missing '=')");
            had_error = 1;
            continue;
        }

        *eq = '\0';
        value = trim_ascii(eq + 1);
        key = trim_ascii(key);

        if (strcmp(key, "screenshot") == 0) {
            if (!value[0]) {
                config->screenshot_mask = 0u;
            } else if (hotkeys_parse_mask(value, &config->screenshot_mask) != 0) {
                config->screenshot_mask = 0u;
                hotkeys_log_parse_error("invalid screenshot binding; disabling it");
                had_error = 1;
            }
            continue;
        }

        if (strcmp(key, "manual") == 0) {
            if (!value[0]) {
                config->manual_mask = 0u;
            } else if (hotkeys_parse_mask(value, &config->manual_mask) != 0) {
                config->manual_mask = 0u;
                hotkeys_log_parse_error("invalid manual binding; disabling it");
                had_error = 1;
            }
            continue;
        }

        hotkeys_log_parse_error("unknown config key; ignoring line");
        had_error = 1;
    }

    fclose(f);
    return had_error ? -1 : 0;
}

int hotkeys_save_config(const varnish_hotkey_config *config) {
    char path[HOTKEYS_MAX_PATH];
    char state_dir[HOTKEYS_MAX_PATH];
    char screenshot[64];
    char manual[64];
    FILE *f;

    if (!config)
        return -1;
    if (hotkeys_get_state_dir(state_dir, sizeof(state_dir)) != 0 ||
        hotkeys_get_config_path(path, sizeof(path)) != 0)
        return -1;
    if (hotkeys_format_mask(config->screenshot_mask, screenshot, sizeof(screenshot)) != 0)
        return -1;
    if (hotkeys_format_mask(config->manual_mask, manual, sizeof(manual)) != 0)
        return -1;

    hotkeys_mkdirp(state_dir);

    f = fopen(path, "wb");
    if (!f)
        return -1;

    fprintf(f, "# Varnish hotkeys\n");
    fprintf(f, "screenshot=%s\n", screenshot);
    fprintf(f, "manual=%s\n", manual);
    fclose(f);
    return 0;
}

void hotkeys_logic_init(varnish_hotkey_logic *logic,
                        uint32_t screenshot_mask,
                        uint32_t manual_mask) {
    if (!logic) return;
    memset(logic, 0, sizeof(*logic));
    logic->screenshot_mask = screenshot_mask;
    logic->manual_mask = manual_mask;
    logic->wait_for_release = true;
}

void hotkeys_logic_set_binding(varnish_hotkey_logic *logic,
                               uint32_t screenshot_mask,
                               uint32_t manual_mask) {
    if (!logic) return;
    logic->screenshot_mask = screenshot_mask;
    logic->manual_mask = manual_mask;
    logic->fired_this_cycle = false;
    logic->wait_for_release = true;
}

void hotkeys_logic_set_paused(varnish_hotkey_logic *logic, bool paused) {
    if (!logic) return;
    logic->paused = paused;
    logic->fired_this_cycle = false;
    logic->wait_for_release = true;
}

varnish_hotkey_action hotkeys_logic_update(varnish_hotkey_logic *logic,
                                           uint32_t pressed_mask) {
    if (!logic)
        return VARNISH_HOTKEY_ACTION_NONE;

    if (pressed_mask == 0u) {
        logic->fired_this_cycle = false;
        logic->wait_for_release = false;
        return VARNISH_HOTKEY_ACTION_NONE;
    }

    if (logic->paused || logic->wait_for_release)
        return VARNISH_HOTKEY_ACTION_NONE;

    if (!logic->fired_this_cycle &&
        logic->screenshot_mask != 0u &&
        pressed_mask == logic->screenshot_mask) {
        logic->fired_this_cycle = true;
        return VARNISH_HOTKEY_ACTION_SCREENSHOT;
    }

    if (!logic->fired_this_cycle &&
        logic->manual_mask != 0u &&
        pressed_mask == logic->manual_mask) {
        logic->fired_this_cycle = true;
        return VARNISH_HOTKEY_ACTION_MANUAL;
    }

    return VARNISH_HOTKEY_ACTION_NONE;
}

#if defined(PLATFORM_TG5040) || defined(PLATFORM_TG5050)
static void hotkeys_trimui_close_joysticks(void) {
    for (int i = 0; i < g_hotkeys.joystick_count; i++) {
        if (g_hotkeys.joysticks[i]) {
            SDL_JoystickClose(g_hotkeys.joysticks[i]);
            g_hotkeys.joysticks[i] = NULL;
        }
    }
    g_hotkeys.joystick_count = 0;
}

static int hotkeys_trimui_open_joysticks(void) {
    int count;
    bool had_joystick_subsystem = SDL_WasInit(SDL_INIT_JOYSTICK) != 0;

    hotkeys_trimui_close_joysticks();

    if (!had_joystick_subsystem && SDL_InitSubSystem(SDL_INIT_JOYSTICK) < 0) {
        fprintf(stderr, "varnish: hotkeys: SDL joystick init failed: %s\n",
                SDL_GetError());
        return -1;
    }
    if (!had_joystick_subsystem)
        g_hotkeys.joystick_subsystem_owned = true;

    if (!g_hotkeys.joystick_event_state_saved) {
        g_hotkeys.joystick_event_state = SDL_JoystickEventState(SDL_QUERY);
        g_hotkeys.joystick_event_state_saved = true;
    }
    SDL_JoystickEventState(SDL_DISABLE);
    count = SDL_NumJoysticks();
    for (int i = 0; i < count && g_hotkeys.joystick_count < HOTKEYS_TRIMUI_MAX_JOYSTICKS; i++) {
        SDL_Joystick *joy = SDL_JoystickOpen(i);
        if (!joy)
            continue;
        g_hotkeys.joysticks[g_hotkeys.joystick_count++] = joy;
    }

    g_hotkeys.joystick_ready = true;
    return g_hotkeys.joystick_count > 0 ? 0 : -1;
}

static void hotkeys_trimui_set_button(uint32_t *mask,
                                      varnish_hotkey_button button,
                                      bool pressed) {
    uint32_t bit = hotkeys_button_bit(button);

    if (!bit) return;
    if (pressed)
        *mask |= bit;
    else
        *mask &= ~bit;
}

static uint32_t hotkeys_trimui_pressed_mask(void) {
    uint32_t mask = 0u;

    if (!g_hotkeys.joystick_ready || g_hotkeys.joystick_count == 0)
        return 0u;

    SDL_JoystickUpdate();

    for (int i = 0; i < g_hotkeys.joystick_count; i++) {
        SDL_Joystick *joy = g_hotkeys.joysticks[i];
        Sint16 axis_x;
        Sint16 axis_y;

        if (!joy || !SDL_JoystickGetAttached(joy))
            continue;

        hotkeys_trimui_set_button(&mask, VARNISH_HOTKEY_BUTTON_A,
                                  SDL_JoystickGetButton(joy, 1) != 0);
        hotkeys_trimui_set_button(&mask, VARNISH_HOTKEY_BUTTON_B,
                                  SDL_JoystickGetButton(joy, 0) != 0);
        hotkeys_trimui_set_button(&mask, VARNISH_HOTKEY_BUTTON_X,
                                  SDL_JoystickGetButton(joy, 3) != 0);
        hotkeys_trimui_set_button(&mask, VARNISH_HOTKEY_BUTTON_Y,
                                  SDL_JoystickGetButton(joy, 2) != 0);
        hotkeys_trimui_set_button(&mask, VARNISH_HOTKEY_BUTTON_L1,
                                  SDL_JoystickGetButton(joy, 4) != 0);
        hotkeys_trimui_set_button(&mask, VARNISH_HOTKEY_BUTTON_R1,
                                  SDL_JoystickGetButton(joy, 5) != 0);
        hotkeys_trimui_set_button(&mask, VARNISH_HOTKEY_BUTTON_SELECT,
                                  SDL_JoystickGetButton(joy, 6) != 0);
        hotkeys_trimui_set_button(&mask, VARNISH_HOTKEY_BUTTON_START,
                                  SDL_JoystickGetButton(joy, 7) != 0);
        hotkeys_trimui_set_button(&mask, VARNISH_HOTKEY_BUTTON_MENU,
                                  SDL_JoystickGetButton(joy, 8) != 0);
        hotkeys_trimui_set_button(&mask, VARNISH_HOTKEY_BUTTON_F1,
                                  SDL_JoystickGetButton(joy, 9) != 0);
        hotkeys_trimui_set_button(&mask, VARNISH_HOTKEY_BUTTON_F2,
                                  SDL_JoystickGetButton(joy, 10) != 0);
        hotkeys_trimui_set_button(&mask, VARNISH_HOTKEY_BUTTON_R2,
                                  SDL_JoystickGetButton(joy, 11) != 0);

        if (SDL_JoystickNumAxes(joy) > 2)
            hotkeys_trimui_set_button(&mask, VARNISH_HOTKEY_BUTTON_L2,
                                      SDL_JoystickGetAxis(joy, 2) > 0);
        if (SDL_JoystickNumAxes(joy) > 5)
            hotkeys_trimui_set_button(&mask, VARNISH_HOTKEY_BUTTON_R2,
                                      SDL_JoystickGetAxis(joy, 5) > 0);

        if (SDL_JoystickNumHats(joy) > 0) {
            Uint8 hat = SDL_JoystickGetHat(joy, 0);
            hotkeys_trimui_set_button(&mask, VARNISH_HOTKEY_BUTTON_UP,
                                      (hat & SDL_HAT_UP) != 0);
            hotkeys_trimui_set_button(&mask, VARNISH_HOTKEY_BUTTON_DOWN,
                                      (hat & SDL_HAT_DOWN) != 0);
            hotkeys_trimui_set_button(&mask, VARNISH_HOTKEY_BUTTON_LEFT,
                                      (hat & SDL_HAT_LEFT) != 0);
            hotkeys_trimui_set_button(&mask, VARNISH_HOTKEY_BUTTON_RIGHT,
                                      (hat & SDL_HAT_RIGHT) != 0);
        }

        axis_x = SDL_JoystickGetAxis(joy, 0);
        axis_y = SDL_JoystickGetAxis(joy, 1);
        hotkeys_trimui_set_button(&mask, VARNISH_HOTKEY_BUTTON_LEFT,
                                  axis_x < -HOTKEYS_AXIS_DEADZONE);
        hotkeys_trimui_set_button(&mask, VARNISH_HOTKEY_BUTTON_RIGHT,
                                  axis_x > HOTKEYS_AXIS_DEADZONE);
        hotkeys_trimui_set_button(&mask, VARNISH_HOTKEY_BUTTON_UP,
                                  axis_y < -HOTKEYS_AXIS_DEADZONE);
        hotkeys_trimui_set_button(&mask, VARNISH_HOTKEY_BUTTON_DOWN,
                                  axis_y > HOTKEYS_AXIS_DEADZONE);
    }

    return mask;
}
#endif

#if defined(PLATFORM_MY355) && defined(__linux__)
static bool hotkeys_linux_key_supported(const unsigned long *bits, int code) {
    size_t word_bits = sizeof(unsigned long) * 8u;
    size_t index = (size_t)code / word_bits;
    unsigned long bit = 1ul << (code % (int)word_bits);
    return (bits[index] & bit) != 0ul;
}

static int hotkeys_open_best_raw_input(char *out_path, size_t out_size) {
    static const int required_keys[] = {
        KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
        KEY_SPACE, KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_LEFTALT,
        KEY_TAB, KEY_BACKSLASH, KEY_PAGEUP, KEY_PAGEDOWN,
        KEY_ENTER, KEY_RIGHTCTRL, KEY_ESC
    };
    unsigned long key_bits[(KEY_MAX / (sizeof(unsigned long) * 8)) + 1];
    int best_fd = -1;
    int best_score = -1;

    if (out_size > 0)
        out_path[0] = '\0';

    for (int i = 0; i < 16; i++) {
        char path[32];
        int fd;
        int score = 0;

        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;

        memset(key_bits, 0, sizeof(key_bits));
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
            close(fd);
            continue;
        }

        for (size_t j = 0; j < sizeof(required_keys) / sizeof(required_keys[0]); j++) {
            if (hotkeys_linux_key_supported(key_bits, required_keys[j]))
                score++;
        }

        if (score > best_score) {
            if (best_fd >= 0)
                close(best_fd);
            best_fd = fd;
            best_score = score;
            str_copy_trunc(out_path, out_size, path);
            if (score == (int)(sizeof(required_keys) / sizeof(required_keys[0])))
                break;
        } else {
            close(fd);
        }
    }

    return best_fd;
}

static varnish_hotkey_button hotkeys_my355_button_from_key(int code) {
    switch (code) {
        case KEY_UP:         return VARNISH_HOTKEY_BUTTON_UP;
        case KEY_DOWN:       return VARNISH_HOTKEY_BUTTON_DOWN;
        case KEY_LEFT:       return VARNISH_HOTKEY_BUTTON_LEFT;
        case KEY_RIGHT:      return VARNISH_HOTKEY_BUTTON_RIGHT;
        case KEY_SPACE:      return VARNISH_HOTKEY_BUTTON_A;
        case KEY_LEFTCTRL:   return VARNISH_HOTKEY_BUTTON_B;
        case KEY_LEFTSHIFT:  return VARNISH_HOTKEY_BUTTON_X;
        case KEY_LEFTALT:    return VARNISH_HOTKEY_BUTTON_Y;
        case KEY_TAB:        return VARNISH_HOTKEY_BUTTON_L1;
        case KEY_BACKSLASH:  return VARNISH_HOTKEY_BUTTON_R1;
        case KEY_PAGEUP:     return VARNISH_HOTKEY_BUTTON_L2;
        case KEY_PAGEDOWN:   return VARNISH_HOTKEY_BUTTON_R2;
        case KEY_ENTER:      return VARNISH_HOTKEY_BUTTON_START;
        case KEY_RIGHTCTRL:  return VARNISH_HOTKEY_BUTTON_SELECT;
        case KEY_ESC:        return VARNISH_HOTKEY_BUTTON_MENU;
        default:             return VARNISH_HOTKEY_BUTTON_NONE;
    }
}

static uint32_t hotkeys_my355_pressed_mask(void) {
    struct input_event ev;
    ssize_t n;

    if (g_hotkeys.raw_input_fd < 0)
        return 0u;

    errno = 0;
    while ((n = read(g_hotkeys.raw_input_fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
        varnish_hotkey_button button;
        uint32_t bit;

        if (ev.type != EV_KEY || ev.value > 1)
            continue;

        button = hotkeys_my355_button_from_key((int)ev.code);
        bit = hotkeys_button_bit(button);
        if (!bit)
            continue;

        if (ev.value == 1)
            g_hotkeys.raw_pressed_mask |= bit;
        else
            g_hotkeys.raw_pressed_mask &= ~bit;
    }

    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        close(g_hotkeys.raw_input_fd);
        g_hotkeys.raw_input_fd = -1;
        g_hotkeys.raw_pressed_mask = 0u;
    }

    return g_hotkeys.raw_pressed_mask;
}
#endif

static uint32_t hotkeys_backend_pressed_mask(void) {
#if defined(PLATFORM_TG5040) || defined(PLATFORM_TG5050)
    uint32_t now = hotkeys_now_ms();

    if (!g_hotkeys.joystick_ready || g_hotkeys.joystick_count == 0) {
        if (now >= g_hotkeys.next_scan_ms) {
            hotkeys_trimui_open_joysticks();
            g_hotkeys.next_scan_ms = now + HOTKEYS_SCAN_INTERVAL_MS;
        }
    }
    return hotkeys_trimui_pressed_mask();
#elif defined(PLATFORM_MY355) && defined(__linux__)
    uint32_t now = hotkeys_now_ms();
    if (g_hotkeys.raw_input_fd < 0) {
        if (now >= g_hotkeys.next_scan_ms) {
            g_hotkeys.raw_input_fd = hotkeys_open_best_raw_input(
                g_hotkeys.raw_input_path, sizeof(g_hotkeys.raw_input_path));
            g_hotkeys.next_scan_ms = now + HOTKEYS_SCAN_INTERVAL_MS;
        }
    }
    return hotkeys_my355_pressed_mask();
#else
    return 0u;
#endif
}

int hotkeys_runtime_init(void) {
    memset(&g_hotkeys, 0, sizeof(g_hotkeys));
#if defined(PLATFORM_MY355) && defined(__linux__)
    g_hotkeys.raw_input_fd = -1;
#endif
    hotkeys_load_config(&g_hotkeys.config);
    hotkeys_logic_init(&g_hotkeys.logic,
                       g_hotkeys.config.screenshot_mask,
                       g_hotkeys.config.manual_mask);
    g_hotkeys.initialized = true;
    return 0;
}

void hotkeys_runtime_cleanup(void) {
    if (!g_hotkeys.initialized)
        return;

#if defined(PLATFORM_TG5040) || defined(PLATFORM_TG5050)
    hotkeys_trimui_close_joysticks();
    if (g_hotkeys.joystick_event_state_saved) {
        SDL_JoystickEventState(g_hotkeys.joystick_event_state);
        g_hotkeys.joystick_event_state_saved = false;
    }
    if (g_hotkeys.joystick_subsystem_owned && SDL_WasInit(SDL_INIT_JOYSTICK))
        SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
#endif
#if defined(PLATFORM_MY355) && defined(__linux__)
    if (g_hotkeys.raw_input_fd >= 0) {
        close(g_hotkeys.raw_input_fd);
        g_hotkeys.raw_input_fd = -1;
    }
#endif
    memset(&g_hotkeys, 0, sizeof(g_hotkeys));
}

int hotkeys_runtime_reload(void) {
    int rc;

    if (!g_hotkeys.initialized)
        return -1;

    rc = hotkeys_load_config(&g_hotkeys.config);
    hotkeys_logic_set_binding(&g_hotkeys.logic,
                              g_hotkeys.config.screenshot_mask,
                              g_hotkeys.config.manual_mask);
    return rc;
}

void hotkeys_runtime_set_paused(bool paused) {
    if (!g_hotkeys.initialized)
        return;
    hotkeys_logic_set_paused(&g_hotkeys.logic, paused);
}

uint32_t hotkeys_runtime_pressed_mask(void) {
    if (!g_hotkeys.initialized)
        return 0u;
    return hotkeys_backend_pressed_mask();
}

varnish_hotkey_action hotkeys_runtime_poll(void) {
    if (!g_hotkeys.initialized)
        return VARNISH_HOTKEY_ACTION_NONE;
    return hotkeys_logic_update(&g_hotkeys.logic, hotkeys_runtime_pressed_mask());
}
