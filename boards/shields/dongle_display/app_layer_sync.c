#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/hid.h>

uint8_t current_app_layer = 0;
uint8_t active_app_layer = 0;

// --- LECTURE DE LA CONFIGURATION DEPUIS LA KEYMAP ---
#define APP_CFG_NODE DT_NODELABEL(app_layer_config)

#if DT_NODE_EXISTS(APP_CFG_NODE) && DT_NODE_HAS_PROP(APP_CFG_NODE, auto_layers)
    #define AUTO_LAYERS_LEN DT_PROP_LEN(APP_CFG_NODE, auto_layers)
    static const uint8_t auto_layers[] = DT_PROP(APP_CFG_NODE, auto_layers);
#else
    #define AUTO_LAYERS_LEN 0
    static const uint8_t auto_layers[] = {0};
#endif

bool is_auto_layer(uint8_t layer) {
    for (int i = 0; i < AUTO_LAYERS_LEN; i++) {
        if (auto_layers[i] == layer) return true;
    }
    return false;
}
// -----------------------------------------------------

// 1. On écoute le Mac pour stocker l'ID de l'application
static int on_hid_indicators(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    
    current_app_layer = ev->indicators;
    
    // Si le Mac revient à 0 (app par défaut), on désactive tout
    if (current_app_layer == 0) {
        if (active_app_layer > 0 && zmk_keymap_layer_active(active_app_layer)) {
            zmk_keymap_layer_deactivate(active_app_layer, true);
        }
        active_app_layer = 0;
    }
    // Si c'est un calque AUTO (ex: Calculatrice), on l'active direct !
    else if (is_auto_layer(current_app_layer)) {
        if (active_app_layer != current_app_layer && active_app_layer > 0 && zmk_keymap_layer_active(active_app_layer)) {
            zmk_keymap_layer_deactivate(active_app_layer, true);
        }
        zmk_keymap_layer_activate(current_app_layer, true);
        active_app_layer = current_app_layer;
    }
    // Si c'est un calque MANUEL (AutoCAD, Word), on l'enregistre juste en mémoire
    else {
        if (active_app_layer > 0 && zmk_keymap_layer_active(active_app_layer)) {
            zmk_keymap_layer_deactivate(active_app_layer, true);
        }
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
        if (current_app_layer > 0 && !is_auto_layer(current_app_layer)) {
            if (zmk_keymap_layer_active(current_app_layer)) {
                zmk_keymap_layer_deactivate(current_app_layer, true);
                active_app_layer = 0;
            } else {
                zmk_keymap_layer_activate(current_app_layer, true);
                active_app_layer = current_app_layer;
            }
        }
        return ZMK_EV_EVENT_HANDLED;
    }

    // 3. One-Shot : Si le calque MANUEL est actif et qu'on tape une vraie touche
    if (active_app_layer > 0 && zmk_keymap_layer_active(active_app_layer)) {
        if (!is_auto_layer(active_app_layer)) {
            bool is_mod = (ev->usage_page == 0x07 && ev->keycode >= 0xE0 && ev->keycode <= 0xE7);
            if (!is_mod) {
                zmk_keymap_layer_deactivate(active_app_layer, true);
                active_app_layer = 0;
            }
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(app_layer_sync_ind, on_hid_indicators);
ZMK_SUBSCRIPTION(app_layer_sync_ind, zmk_hid_indicators_changed);

ZMK_LISTENER(app_layer_sync_key, on_keycode_state_changed);
ZMK_SUBSCRIPTION(app_layer_sync_key, zmk_keycode_state_changed);