#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/keymap.h>

// Variable globale pour stocker l'ID de l'application active
uint8_t current_app_layer = 0;

static int on_hid_indicators(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    if (ev == NULL) {
        return 0;
    }

    // Le Mac envoie l'ID de l'application. On le stocke sans changer de calque.
    current_app_layer = ev->indicators;

    return 0;
}

ZMK_LISTENER(app_layer_sync, on_hid_indicators);
ZMK_SUBSCRIPTION(app_layer_sync, zmk_hid_indicators_changed);