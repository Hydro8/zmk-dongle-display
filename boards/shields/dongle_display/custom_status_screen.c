/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "custom_status_screen.h"
#include "widgets/battery_status.h"
#include "widgets/modifiers.h"
#include "widgets/layer_status.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <zmk/split/central.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

extern uint8_t current_app_layer;

#if IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_LAYER)
static struct zmk_widget_layer_status layer_status_widget;
#endif

#if IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_MODIFIERS)
static struct zmk_widget_modifiers modifiers_widget;
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY)
static struct zmk_widget_dongle_battery_status dongle_battery_status_widget;
#endif

lv_style_t global_style;
lv_obj_t *app_label;

extern int aeklipse_get_connected_sources(uint8_t *sources);

#define AEK_DISPLAY_POLL_MS 500
#define AEK_DISPLAY_DISCONNECTED_TIMEOUT_MS 30000

static const struct device *aek_display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static uint32_t disconnected_ms;
static bool display_is_blank;

static void set_display_blank(bool blank) {
    if (blank == display_is_blank || aek_display == NULL || !device_is_ready(aek_display)) {
        return;
    }

    int rc = blank ? display_blanking_on(aek_display) : display_blanking_off(aek_display);
    if (rc == 0) {
        display_is_blank = blank;
    }
}

// Supervise l'affichage sans endormir le dongle.
// 1+ moitié connectée : OLED ON.
// 0 moitié pendant 30 s : OLED OFF.
static void update_app_label(lv_timer_t *timer) {
    ARG_UNUSED(timer);

    lv_label_set_text_fmt(app_label, "%d", current_app_layer);

    uint8_t sources[ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT];
    int connected_count = aeklipse_get_connected_sources(sources);

#if IS_ENABLED(CONFIG_ZMK_BATTERY)
    zmk_widget_dongle_battery_status_set_connected_sources(
        sources, connected_count > 0 ? (size_t)connected_count : 0);
#endif

    if (connected_count > 0) {
        disconnected_ms = 0;
        set_display_blank(false);
        return;
    }

    if (disconnected_ms < AEK_DISPLAY_DISCONNECTED_TIMEOUT_MS) {
        disconnected_ms += AEK_DISPLAY_POLL_MS;
    }

    if (disconnected_ms >= AEK_DISPLAY_DISCONNECTED_TIMEOUT_MS) {
        set_display_blank(true);
    }
}

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen;
    screen = lv_obj_create(NULL);
    
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    lv_style_init(&global_style);
    lv_style_set_bg_color(&global_style, lv_color_white());
    lv_style_set_bg_opa(&global_style, LV_OPA_COVER);
    lv_style_set_text_color(&global_style, lv_color_black());
    lv_style_set_text_font(&global_style, &lv_font_unscii_8);
    lv_style_set_text_letter_space(&global_style, 1);
    lv_style_set_text_line_space(&global_style, 1);
    lv_obj_add_style(screen, &global_style, LV_PART_MAIN);

#if IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_LAYER)
    zmk_widget_layer_status_init(&layer_status_widget, screen);
    lv_obj_align(zmk_widget_layer_status_obj(&layer_status_widget), LV_ALIGN_TOP_MID, 0, 0);
#endif

#if IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_MODIFIERS)
    zmk_widget_modifiers_init(&modifiers_widget, screen);
    lv_obj_align(zmk_widget_modifiers_obj(&modifiers_widget), LV_ALIGN_TOP_MID, 0, 35);
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY)
    zmk_widget_dongle_battery_status_init(&dongle_battery_status_widget, screen);
    lv_obj_align(zmk_widget_dongle_battery_status_obj(&dongle_battery_status_widget), LV_ALIGN_BOTTOM_MID, 0, 0);
#endif

    // Le texte de debug en bas au centre
    app_label = lv_label_create(screen);
    lv_obj_align(app_label, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_label_set_text(app_label, "0");
    
    // Lance le minuteur de mise à jour
    lv_timer_create(update_app_label, AEK_DISPLAY_POLL_MS, NULL);

    return screen;
}