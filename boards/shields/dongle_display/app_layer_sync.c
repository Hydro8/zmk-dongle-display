#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/keymap.h>

#define SCROLL_LOCK_BIT (1 << 2)

static int on_hid_indicators(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    if (ev == NULL) {
        return 0;
    }

    // Si Scroll Lock est allumé, on active le calque 2 (ex: Photoshop)
    if (ev->indicators & SCROLL_LOCK_BIT) {
        zmk_keymap_layer_activate(2);
    } else {
        // Sinon, on le désactive et on revient au calque par défaut
        zmk_keymap_layer_deactivate(2);
        zmk_keymap_layer_activate(0);
    }

    return 0;
}

ZMK_LISTENER(app_layer_sync, on_hid_indicators);
ZMK_SUBSCRIPTION(app_layer_sync, zmk_hid_indicators_changed);