#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/keymap.h>

static uint8_t current_app_layer = 0;

static int on_hid_indicators(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    if (ev == NULL) {
        return 0;
    }

    // Le Mac envoie directement le numéro du calque (0 = par défaut, 4 = Photoshop, 5 = VSCode...)
    uint8_t target_layer = ev->indicators;

    // Si l'appli n'a pas changé, on ne fait rien
    if (target_layer == current_app_layer) {
        return 0;
    }

    // On désactive l'ancien calque d'appli
    if (current_app_layer != 0) {
        zmk_keymap_layer_deactivate(current_app_layer);
    }

    // On active le nouveau calque (si ce n'est pas 0)
    if (target_layer != 0) {
        zmk_keymap_layer_activate(target_layer);
    }

    current_app_layer = target_layer;

    return 0;
}

ZMK_LISTENER(app_layer_sync, on_hid_indicators);
ZMK_SUBSCRIPTION(app_layer_sync, zmk_hid_indicators_changed);