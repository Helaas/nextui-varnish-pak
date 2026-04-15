/*
 * ui.c — Apostrophe-based management UI for Varnish.
 */

#include "ui.h"

#include "control.h"
#include "hotkeys.h"
#include "ipc.h"
#include "strutil.h"

#include "apostrophe.h"
#include "apostrophe_widgets.h"

#include <stdio.h>
#include <string.h>

static void show_message(const char *message) {
    ap_footer_item footer[] = {
        { .button = AP_BTN_A, .label = "OK", .is_confirm = true },
    };
    ap_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 1,
    };
    ap_confirm_result result = {0};
    (void)ap_confirmation(&opts, &result);
}

static void show_error(const char *message) {
    show_message(message);
}

static bool show_confirm(const char *message, const char *confirm_label) {
    ap_footer_item footer[] = {
        { .button = AP_BTN_B, .label = "Cancel" },
        { .button = AP_BTN_A, .label = confirm_label, .is_confirm = true },
    };
    ap_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 2,
    };
    ap_confirm_result result = {0};
    int rc = ap_confirmation(&opts, &result);
    return rc == AP_OK && result.confirmed;
}

static bool show_enable_reboot_prompt(void) {
    ap_selection_option options[] = {
        { .label = "Later", .value = "later" },
        { .label = "Reboot now", .value = "reboot" },
    };
    ap_footer_item footer[] = {
        { .button = AP_BTN_B, .label = "Later" },
        { .button = AP_BTN_LEFT, .label = "Change", .button_text = "\xE2\x86\x90/\xE2\x86\x92" },
        { .button = AP_BTN_A, .label = "Choose", .is_confirm = true },
    };
    ap_selection_result result = {0};
    int rc = ap_selection("Varnish enabled.\n\nReboot now to inject LD_PRELOAD into the current launcher session.",
                          options, 2, footer, 3, &result);

    return rc == AP_OK && result.selected_index == 1;
}

static void show_state_error(const char *prefix, const varnish_status *status) {
    char state[160];
    char message[320];

    control_format_status(status, state, sizeof(state));
    snprintf(message, sizeof(message), "%s\n\n%s", prefix, state);
    show_error(message);
}

static varnish_hotkey_button ui_hotkey_button_from_ap(ap_button button) {
    switch (button) {
        case AP_BTN_UP:     return VARNISH_HOTKEY_BUTTON_UP;
        case AP_BTN_DOWN:   return VARNISH_HOTKEY_BUTTON_DOWN;
        case AP_BTN_LEFT:   return VARNISH_HOTKEY_BUTTON_LEFT;
        case AP_BTN_RIGHT:  return VARNISH_HOTKEY_BUTTON_RIGHT;
        case AP_BTN_A:      return VARNISH_HOTKEY_BUTTON_A;
        case AP_BTN_B:      return VARNISH_HOTKEY_BUTTON_B;
        case AP_BTN_X:      return VARNISH_HOTKEY_BUTTON_X;
        case AP_BTN_Y:      return VARNISH_HOTKEY_BUTTON_Y;
        case AP_BTN_L1:     return VARNISH_HOTKEY_BUTTON_L1;
        case AP_BTN_L2:     return VARNISH_HOTKEY_BUTTON_L2;
        case AP_BTN_R1:     return VARNISH_HOTKEY_BUTTON_R1;
        case AP_BTN_R2:     return VARNISH_HOTKEY_BUTTON_R2;
        case AP_BTN_START:  return VARNISH_HOTKEY_BUTTON_START;
        case AP_BTN_SELECT: return VARNISH_HOTKEY_BUTTON_SELECT;
        case AP_BTN_MENU:   return VARNISH_HOTKEY_BUTTON_MENU;
        default:            return VARNISH_HOTKEY_BUTTON_NONE;
    }
}

static void ui_format_binding(uint32_t mask, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    if (mask == 0u || hotkeys_format_mask(mask, out, out_size) != 0 || !out[0])
        snprintf(out, out_size, "Disabled");
}

static void ui_pause_hotkeys(bool *paused) {
    if (!paused) return;
    if (ipc_daemon_running() && ipc_hotkeys_pause() == 0)
        *paused = true;
}

static void ui_resume_hotkeys(bool paused) {
    if (!paused) return;
    (void)ipc_hotkeys_resume();
}

static int ui_capture_hotkey(uint32_t *out_mask) {
    uint32_t ap_mask = 0u;
    uint32_t current_mask = 0u;
    uint32_t candidate_mask = 0u;
    uint32_t b_bit = hotkeys_button_bit(VARNISH_HOTKEY_BUTTON_B);
    bool pending_cancel_b = false;
    bool runtime_ready = false;
    char current_text[64];
    char status_text[128];
    ap_footer_item footer[] = {
        { .button = AP_BTN_B, .label = "Cancel" },
    };

    if (!out_mask)
        return AP_CANCELLED;

    runtime_ready = (hotkeys_runtime_init() == 0);
    if (runtime_ready)
        current_mask = hotkeys_runtime_pressed_mask();

    for (;;) {
        uint32_t pressed_mask;
        uint32_t newly_pressed;

        while (1) {
            ap_input_event ev;
            varnish_hotkey_button hotkey_button;
            uint32_t bit;

            if (!ap_poll_input(&ev))
                break;
            if (ev.repeated)
                continue;

            hotkey_button = ui_hotkey_button_from_ap(ev.button);
            bit = hotkeys_button_bit(hotkey_button);

            if (ev.pressed) {
                ap_mask |= bit;
            } else if (bit) {
                ap_mask &= ~bit;
            }
        }

        pressed_mask = ap_mask;
        if (runtime_ready)
            pressed_mask |= hotkeys_runtime_pressed_mask();

        newly_pressed = pressed_mask & ~current_mask;
        if (newly_pressed != 0u) {
            if (current_mask == 0u && candidate_mask == 0u && newly_pressed == b_bit)
                pending_cancel_b = true;

            candidate_mask |= newly_pressed;
            if ((newly_pressed & ~b_bit) != 0u)
                pending_cancel_b = false;
        }

        if (pressed_mask == 0u && current_mask != 0u && candidate_mask != 0u) {
            int count = hotkeys_mask_button_count(candidate_mask);

            if (pending_cancel_b && candidate_mask == b_bit) {
                if (runtime_ready)
                    hotkeys_runtime_cleanup();
                return AP_CANCELLED;
            }

            pending_cancel_b = false;
            if (count >= 2 && count <= 4) {
                *out_mask = candidate_mask;
                if (runtime_ready)
                    hotkeys_runtime_cleanup();
                return AP_OK;
            }

            candidate_mask = 0u;
        }
        current_mask = pressed_mask;

        ui_format_binding(candidate_mask, current_text, sizeof(current_text));
        if (candidate_mask == 0u) {
            snprintf(current_text, sizeof(current_text), "Waiting");
            snprintf(status_text, sizeof(status_text),
                     "Press 2-4 buttons together, then release them to save.");
        } else if (hotkeys_mask_button_count(candidate_mask) > 4) {
            snprintf(status_text, sizeof(status_text),
                     "Too many buttons. Release everything and try again.");
        } else if (hotkeys_mask_button_count(candidate_mask) < 2) {
            snprintf(status_text, sizeof(status_text),
                     "Need 2-4 buttons together. Current: %s", current_text);
        } else {
            snprintf(status_text, sizeof(status_text),
                     "Release to save. Current: %s", current_text);
        }

        ap_clear_screen();
        ap_draw_screen_title("Capture Hotkey", NULL);
        {
            SDL_Rect content = ap_get_content_rect(true, true, false);
            int pad = ap_scale(12);
            int text_y = content.y + pad;
            int text_w = content.w - pad * 2;
            TTF_Font *body_font = ap_get_font(AP_FONT_SMALL);
            TTF_Font *value_font = ap_get_font(AP_FONT_LARGE);
            ap_theme *theme = ap_get_theme();

            ap_draw_text_wrapped(body_font, status_text, content.x + pad, text_y, text_w,
                                 theme->text, AP_ALIGN_LEFT);
            text_y += ap_measure_wrapped_text_height(body_font, status_text, text_w) + ap_scale(16);
            ap_draw_text(value_font, current_text, content.x + pad, text_y, theme->accent);
        }
        ap_draw_footer(footer, 1);
        ap_request_frame_in(16);
        ap_present();
    }
}

static void ui_run_hotkeys_menu(void) {
    varnish_hotkey_config draft;

    hotkeys_load_config(&draft);

    for (;;) {
        char screenshot_text[64];
        char manual_text[64];
        char record_text[64];
        char help_text[256];
        ap_option screenshot_value[] = {
            { .label = screenshot_text, .value = screenshot_text },
        };
        ap_option manual_value[] = {
            { .label = manual_text, .value = manual_text },
        };
        ap_option record_value[] = {
            { .label = record_text, .value = record_text },
        };
        ap_options_item items[] = {
            {
                .label = "Screenshot",
                .type = AP_OPT_CLICKABLE,
                .options = screenshot_value,
                .option_count = 1,
                .selected_option = 0,
            },
            {
                .label = "Manual",
                .type = AP_OPT_CLICKABLE,
                .options = manual_value,
                .option_count = 1,
                .selected_option = 0,
            },
            {
                .label = "Record Video",
                .type = AP_OPT_CLICKABLE,
                .options = record_value,
                .option_count = 1,
                .selected_option = 0,
            },
        };
        ap_footer_item footer[] = {
            { .button = AP_BTN_B, .label = "Back" },
            { .button = AP_BTN_X, .label = "Clear" },
            { .button = AP_BTN_A, .label = "Edit" },
            { .button = AP_BTN_START, .label = "Save", .is_confirm = true },
        };
        ap_options_list_opts opts;
        ap_options_list_result result = {0};
        uint32_t captured_mask = 0u;
        int rc;

        ui_format_binding(draft.screenshot_mask, screenshot_text, sizeof(screenshot_text));
        ui_format_binding(draft.manual_mask, manual_text, sizeof(manual_text));
        ui_format_binding(draft.record_mask, record_text, sizeof(record_text));
        snprintf(help_text, sizeof(help_text),
                 "Define global button chords.\n\nScreenshot: %s\nManual: %s\nRecord Video: %s",
                 screenshot_text, manual_text, record_text);

        opts = (ap_options_list_opts) {
            .title = "Hotkeys",
            .items = items,
            .item_count = 3,
            .footer = footer,
            .footer_count = 4,
            .action_button = AP_BTN_X,
            .confirm_button = AP_BTN_START,
            .help_text = help_text,
            .label_font = ap_get_font(AP_FONT_MEDIUM),
        };

        rc = ap_options_list(&opts, &result);
        if (rc != AP_OK)
            return;

        if (result.action == AP_ACTION_TRIGGERED) {
            if (result.focused_index == 0)
                draft.screenshot_mask = 0u;
            else if (result.focused_index == 1)
                draft.manual_mask = 0u;
            else if (result.focused_index == 2)
                draft.record_mask = 0u;
            continue;
        }

        if (result.action == AP_ACTION_CONFIRMED) {
            if (hotkeys_save_config(&draft) != 0) {
                show_error("Could not save hotkeys.");
                continue;
            }
            if (ipc_daemon_running())
                (void)ipc_hotkeys_reload();
            show_message("Hotkeys saved.");
            return;
        }

        if (result.action == AP_ACTION_SELECTED) {
            if (ui_capture_hotkey(&captured_mask) == AP_OK) {
                if (result.focused_index == 0)
                    draft.screenshot_mask = captured_mask;
                else if (result.focused_index == 1)
                    draft.manual_mask = captured_mask;
                else if (result.focused_index == 2)
                    draft.record_mask = captured_mask;
            }
        }
    }
}

int ui_run(const char *self_path) {
    bool hotkeys_paused = false;
    varnish_status initial_status;
    bool want_enabled;

    control_get_status(&initial_status);
    want_enabled = control_is_enabled(&initial_status);
    ui_pause_hotkeys(&hotkeys_paused);

    for (;;) {
        varnish_status status;
        varnish_hotkey_config hotkey_config;
        char screenshot_text[64];
        char manual_text[64];
        char record_text[64];
        char help_text[384];
        ap_option enabled_options[] = {
            { .label = "Off", .value = "0" },
            { .label = "On",  .value = "1" },
        };
        ap_option hotkey_value[] = {
            { .label = screenshot_text, .value = screenshot_text },
        };
        ap_options_item items[] = {
            {
                .label = "Enabled",
                .type = AP_OPT_STANDARD,
                .options = enabled_options,
                .option_count = 2,
                .selected_option = 0,
            },
            {
                .label = "Hotkeys",
                .type = AP_OPT_CLICKABLE,
                .options = hotkey_value,
                .option_count = 1,
                .selected_option = 0,
            },
        };
        ap_footer_item footer[] = {
            { .button = AP_BTN_B, .label = "Back" },
            { .button = AP_BTN_LEFT, .label = "Change", .button_text = "←/→" },
            { .button = AP_BTN_A, .label = "Open" },
            { .button = AP_BTN_START, .label = "Save", .is_confirm = true },
        };
        ap_options_list_opts opts;
        ap_options_list_result result = {0};
        int rc;

        control_get_status(&status);
        hotkeys_load_config(&hotkey_config);
        ui_format_binding(hotkey_config.screenshot_mask,
                          screenshot_text, sizeof(screenshot_text));
        ui_format_binding(hotkey_config.manual_mask, manual_text, sizeof(manual_text));
        ui_format_binding(hotkey_config.record_mask, record_text, sizeof(record_text));

        items[0].selected_option = want_enabled ? 1 : 0;
        control_format_status(&status, help_text, sizeof(help_text));
        str_append(help_text, sizeof(help_text), "\nScreenshot: ");
        str_append(help_text, sizeof(help_text), screenshot_text);
        str_append(help_text, sizeof(help_text), "\nManual: ");
        str_append(help_text, sizeof(help_text), manual_text);
        str_append(help_text, sizeof(help_text), "\nRecord Video: ");
        str_append(help_text, sizeof(help_text), record_text);

        opts = (ap_options_list_opts) {
            .title = "Varnish",
            .items = items,
            .item_count = 2,
            .footer = footer,
            .footer_count = 4,
            .confirm_button = AP_BTN_START,
            .help_text = help_text,
            .label_font = ap_get_font(AP_FONT_MEDIUM),
        };

        rc = ap_options_list(&opts, &result);
        if (rc != AP_OK)
            break;

        want_enabled = (result.items[0].selected_option == 1);

        if (result.action == AP_ACTION_SELECTED && result.focused_index == 1) {
            ui_run_hotkeys_menu();
            continue;
        }

        if (result.action != AP_ACTION_CONFIRMED)
            continue;

        if (!want_enabled) {
            if (!show_confirm("Disable Varnish?\n\nThis removes the startup patch and boot hook, then stops the running daemon.\nReboot to fully unload the current launcher session.",
                              "Disable")) {
                continue;
            }
            if (control_disable(&status) != 0) {
                show_state_error("Could not fully disable Varnish.", &status);
            } else {
                show_message("Varnish disabled.\n\nReboot to fully unload the current launcher session.");
            }
            continue;
        }

        if (control_enable(self_path, &status) != 0) {
            show_state_error("Could not fully enable Varnish.", &status);
        } else {
            ui_pause_hotkeys(&hotkeys_paused);
            if (show_enable_reboot_prompt() && control_request_reboot() != 0)
                show_error("Could not request reboot.");
        }
    }

    ui_resume_hotkeys(hotkeys_paused);
    return 0;
}
