#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/keymap.h>

static int on_hid_indicators(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    if (ev == NULL) {
        return 0;
    }

    // Le Mac envoie directement le numéro du calque (0 = par défaut, 8 = Photoshop, etc.)
    uint8_t target_layer = ev->indicators;

    // On active le nouveau calque (et désactive les autres automatiquement)
    zmk_keymap_layer_to(target_layer);

    return 0;
}

ZMK_LISTENER(app_layer_sync, on_hid_indicators);
ZMK_SUBSCRIPTION(app_layer_sync, zmk_hid_indicators_changed);