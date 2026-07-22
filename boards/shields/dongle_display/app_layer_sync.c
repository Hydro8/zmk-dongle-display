#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/hid.h>
#include <zmk/usb.h>

uint8_t current_app_layer = 0;
uint8_t active_app_layer = 0;
bool is_layer_persistent = false;

// Fonction appelée quand le Mac envoie des données sur le canal Raw HID
void zmk_app_layer_receive(const uint8_t *data, size_t len) {
    if (len < 1) return;
    
    uint8_t received = data[0];
    bool is_one_shot = (received & 0x80) != 0;
    bool is_auto = (received & 0x40) != 0;
    uint8_t layer = received & 0x3F;
    
    if (active_app_layer > 0 && zmk_keymap_layer_active(active_app_layer)) {
        zmk_keymap_layer_deactivate(active_app_layer, true);
    }
    active_app_layer = 0;
    is_layer_persistent = false;
    
    if (layer == 0) {
        current_app_layer = 0;
    } else if (is_auto) {
        zmk_keymap_layer_activate(layer, true);
        active_app_layer = layer;
        current_app_layer = layer;
        is_layer_persistent = !is_one_shot;
    } else {
        current_app_layer = layer;
        is_layer_persistent = !is_one_shot;
    }
}

// 2. On écoute les touches du clavier (inchangé)
static int on_keycode_state_changed(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->usage_page == 0x07 && ev->keycode == 0x68) { // F13
        if (current_app_layer > 0 && active_app_layer == 0) {
            zmk_keymap_layer_activate(current_app_layer, true);
            active_app_layer = current_app_layer;
        } else if (active_app_layer > 0) {
            zmk_keymap_layer_deactivate(active_app_layer, true);
            active_app_layer = 0;
        }
        return ZMK_EV_EVENT_HANDLED;
    }

    if (active_app_layer > 0 && !is_layer_persistent) {
        bool is_mod = (ev->usage_page == 0x07 && ev->keycode >= 0xE0 && ev->keycode <= 0xE7);
        if (!is_mod) {
            zmk_keymap_layer_deactivate(active_app_layer, true);
            active_app_layer = 0;
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(app_layer_sync_key, on_keycode_state_changed);
ZMK_SUBSCRIPTION(app_layer_sync_key, zmk_keycode_state_changed);