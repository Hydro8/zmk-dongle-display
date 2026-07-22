#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/hid.h>

uint8_t current_app_layer = 0;
uint8_t active_app_layer = 0; // Pour savoir ce qui est réellement activé

// 1. On écoute le Mac pour stocker l'ID de l'application
static int on_hid_indicators(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    
    current_app_layer = ev->indicators;
    
    // Si le Mac revient à 0 (app par défaut) et qu'un calque était actif, on le désactive
    if (current_app_layer == 0 && active_app_layer > 0) {
        zmk_keymap_layer_deactivate(active_app_layer, true);
        active_app_layer = 0;
    }
    
    return ZMK_EV_EVENT_BUBBLE;
}

// 2. On écoute les touches du clavier
static int on_keycode_state_changed(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    // Si on appuie sur F13 (Usage Page 0x07, Keycode 0x68)
    if (ev->usage_page == 0x07 && ev->keycode == 0x68) {
        if (current_app_layer > 0) {
            // Bascule
            if (zmk_keymap_layer_active(current_app_layer)) {
                zmk_keymap_layer_deactivate(current_app_layer, true);
                active_app_layer = 0;
            } else {
                zmk_keymap_layer_activate(current_app_layer, true);
                active_app_layer = current_app_layer;
            }
        }
        return ZMK_EV_EVENT_HANDLED; // Avale la touche F13
    }

    // 3. One-Shot : Si le calque d'appli est actif et qu'on tape une vraie touche
    if (active_app_layer > 0 && zmk_keymap_layer_active(active_app_layer)) {
        bool is_mod = (ev->usage_page == 0x07 && ev->keycode >= 0xE0 && ev->keycode <= 0xE7);
        
        if (!is_mod) {
            // On désactive le calque. La touche en cours sera quand même envoyée au Mac.
            zmk_keymap_layer_deactivate(active_app_layer, true);
            active_app_layer = 0;
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(app_layer_sync_ind, on_hid_indicators);
ZMK_SUBSCRIPTION(app_layer_sync_ind, zmk_hid_indicators_changed);

ZMK_LISTENER(app_layer_sync_key, on_keycode_state_changed);
ZMK_SUBSCRIPTION(app_layer_sync_key, zmk_keycode_state_changed);