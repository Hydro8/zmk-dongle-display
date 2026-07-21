#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/keymap.h>

// On retient le calque d'appli actuellement actif pour pouvoir le désactiver
static uint8_t current_app_layer = 0;

static int on_hid_indicators(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    if (ev == NULL) {
        return 0;
    }

    // Le Mac envoie un numéro d'application (0 = par défaut, 1 = Photoshop, 2 = VSCode, etc.)
    uint8_t app_id = ev->indicators;

    // Si l'appli n'a pas changé, on ne fait rien
    if (app_id == current_app_layer) {
        return 0;
    }

    // On désactive l'ancien calque d'appli
    if (current_app_layer != 0) {
        zmk_keymap_layer_deactivate(current_app_layer);
    }

    // On active le nouveau calque (si ce n'est pas 0)
    if (app_id != 0) {
        zmk_keymap_layer_activate(app_id);
    }

    current_app_layer = app_id;

    return 0;
}

ZMK_LISTENER(app_layer_sync, on_hid_indicators);
ZMK_SUBSCRIPTION(app_layer_sync, zmk_hid_indicators_changed);