#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/hid.h>

uint8_t current_app_layer = 0;

// 1. On écoute le Mac pour stocker l'ID de l'application
static int on_hid_indicators(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    if (ev == NULL) {
        return 0;
    }
    current_app_layer = ev->indicators;
    return 0;
}

// 2. On écoute les touches du clavier pour intercepter F13
static int on_keycode_state_changed(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL || !ev->state) {
        return 0;
    }

    // Si on appuie sur F13 (Usage Page 0x07, Keycode 0x68)
    if (ev->usage_page == 0x07 && ev->keycode == 0x68) {
        if (current_app_layer > 0) {
            // Bascule : si actif, désactive ; si inactif, active
            if (zmk_keymap_layer_active(current_app_layer)) {
                zmk_keymap_layer_deactivate(current_app_layer);
            } else {
                zmk_keymap_layer_activate(current_app_layer);
            }
        }
        return ZMK_EV_EVENT_HANDLED; // Avale la touche pour ne pas l'envoyer au Mac
    }

    return 0;
}

ZMK_LISTENER(app_layer_sync_ind, on_hid_indicators);
ZMK_SUBSCRIPTION(app_layer_sync_ind, zmk_hid_indicators_changed);

ZMK_LISTENER(app_layer_sync_key, on_keycode_state_changed);
ZMK_SUBSCRIPTION(app_layer_sync_key, zmk_keycode_state_changed);