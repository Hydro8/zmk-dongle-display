#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <dt-bindings/zmk/keys.h>

// On récupère la variable de l'autre fichier
extern uint8_t current_app_layer;

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int behavior_app_layer_init(const struct device *dev) { return 0; };

// Comportement Bascule (Toggle) : un appui active, un second appui désactive
static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    if (current_app_layer > 0) {
        if (zmk_keymap_layer_active(current_app_layer)) {
            return zmk_keymap_layer_deactivate(current_app_layer, true);
        } else {
            return zmk_keymap_layer_activate(current_app_layer, true);
        }
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_app_layer_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

// --- ÉCOUTEUR POUR DÉSACTIVER LE CALQUE APRÈS UNE FRAPPE ---
static int on_keycode_state_changed(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL) {
        return 0;
    }

    // Si c'est un appui (state = true) et que le calque d'appli est actif
    if (ev->state && current_app_layer > 0 && zmk_keymap_layer_active(current_app_layer)) {
        
        // On vérifie si la touche est un modificateur (Cmd, Shift, Alt, Ctrl)
        // Les modificateurs sont dans la page 0x07 et vont de 0xE0 à 0xE7
        bool is_mod = (ev->usage_page == HID_USAGE_KEY && ev->keycode >= 0xE0 && ev->keycode <= 0xE7);
        
        // Si ce n'est PAS un modificateur, on désactive le calque
        if (!is_mod) {
            zmk_keymap_layer_deactivate(current_app_layer, true);
        }
    }
    return 0;
}

// On enregistre l'écouteur pour qu'il tourne en arrière-plan
ZMK_LISTENER(app_layer_auto_off, on_keycode_state_changed);
ZMK_SUBSCRIPTION(app_layer_auto_off, zmk_keycode_state_changed);

#define KP_INST(n)                                                                                   \
    DEVICE_DT_INST_DEFINE(n, behavior_app_layer_init, NULL, NULL, NULL, POST_KERNEL,                 \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_app_layer_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KP_INST)

#endif