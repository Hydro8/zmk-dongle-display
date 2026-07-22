#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/hid.h>

uint8_t current_app_layer = 0;
uint8_t active_app_layer = 0;
bool is_layer_persistent = false;

// 1. On écoute le Mac
static int on_hid_indicators(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    
    uint8_t received = ev->indicators;
    
    // Décode les bits
    bool is_one_shot = (received & 0x80) != 0; // Bit 7 (128)
    bool is_auto = (received & 0x40) != 0;     // Bit 6 (64)
    uint8_t layer = received & 0x3F;           // Bits 0 à 5 (0 à 63)
    
    // On désactive l'ancien calque s'il est actif
    if (active_app_layer > 0 && zmk_keymap_layer_active(active_app_layer)) {
        zmk_keymap_layer_deactivate(active_app_layer, true);
    }
    active_app_layer = 0;
    is_layer_persistent = false;
    
    if (layer == 0) {
        // Retour à la base
        current_app_layer = 0;
    } else if (is_auto) {
        // Calque automatique : on l'active direct !
        zmk_keymap_layer_activate(layer, true);
        active_app_layer = layer;
        current_app_layer = layer;
        is_layer_persistent = !is_one_shot; // Si One-Shot, alors non persistant
    } else {
        // Calque manuel : on le stocke en mémoire
        current_app_layer = layer;
        is_layer_persistent = !is_one_shot;
    }
    
    return ZMK_EV_EVENT_BUBBLE;
}

// 2. On écoute les touches du clavier
static int on_keycode_state_changed(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    // Appui sur F13 (0x68)
    if (ev->usage_page == 0x07 && ev->keycode == 0x68) {
        if (current_app_layer > 0 && active_app_layer == 0) {
            zmk_keymap_layer_activate(current_app_layer, true);
            active_app_layer = current_app_layer;
        } else if (active_app_layer > 0) {
            zmk_keymap_layer_deactivate(active_app_layer, true);
            active_app_layer = 0;
        }
        return ZMK_EV_EVENT_HANDLED;
    }

    // 3. One-Shot : Désactive le calque après une frappe (UNIQUEMENT si non persistant)
    if (active_app_layer > 0 && !is_layer_persistent) {
        bool is_mod = (ev->usage_page == 0x07 && ev->keycode >= 0xE0 && ev->keycode <= 0xE7);
        if (!is_mod) {
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