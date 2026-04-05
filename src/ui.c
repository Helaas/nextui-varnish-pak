/*
 * ui.c — Apostrophe-based management UI for Varnish.
 */

#include "ui.h"

#include "control.h"

#include "apostrophe.h"
#include "apostrophe_widgets.h"

#include <stdio.h>

static void show_error(const char *message) {
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

static void show_state_error(const char *prefix, const varnish_status *status) {
    char state[160];
    char message[320];

    control_format_status(status, state, sizeof(state));
    snprintf(message, sizeof(message), "%s\n\n%s", prefix, state);
    show_error(message);
}

int ui_run(const char *self_path) {
    for (;;) {
        varnish_status status;
        char help_text[160];
        ap_option enabled_options[] = {
            { .label = "Off", .value = "0" },
            { .label = "On",  .value = "1" },
        };
        ap_options_item items[] = {
            {
                .label = "Enabled",
                .type = AP_OPT_STANDARD,
                .options = enabled_options,
                .option_count = 2,
                .selected_option = 0,
            },
        };
        ap_footer_item footer[] = {
            { .button = AP_BTN_B, .label = "Back" },
            { .button = AP_BTN_LEFT, .label = "Change", .button_text = "←/→" },
            { .button = AP_BTN_A, .label = "Save", .is_confirm = true },
        };
        ap_options_list_opts opts;
        ap_options_list_result result = {0};
        int rc;
        bool want_enabled;

        control_get_status(&status);
        items[0].selected_option = control_is_enabled(&status) ? 1 : 0;
        control_format_status(&status, help_text, sizeof(help_text));

        opts = (ap_options_list_opts) {
            .title = "Varnish",
            .items = items,
            .item_count = 1,
            .footer = footer,
            .footer_count = 3,
            .confirm_button = AP_BTN_A,
            .help_text = help_text,
            .label_font = ap_get_font(AP_FONT_MEDIUM),
        };

        rc = ap_options_list(&opts, &result);
        if (rc != AP_OK)
            return 0;

        want_enabled = (result.items[0].selected_option == 1);

        if (!want_enabled) {
            if (!show_confirm("Disable Varnish?\n\nThis removes the boot hook and stops the running daemon immediately.\nThe preload wrapper stays installed.",
                              "Disable")) {
                continue;
            }
            if (control_disable(&status) != 0)
                show_state_error("Could not fully disable Varnish.", &status);
            continue;
        }

        if (control_enable(self_path, &status) != 0)
            show_state_error("Could not fully enable Varnish.", &status);
    }
}
