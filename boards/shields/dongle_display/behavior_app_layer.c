/*
 * behavior_app_layer.c
 *
 * Custom ZMK behavior that toggles the application-specific layer.
 * Bound in the keymap with &app_layer on a thumb key.
 *
 * This behavior reads the shared active_app_layer variable (defined in
 * app_layer_sync.c) to know WHICH layer to toggle, then activates or
 * deactivates that layer via the ZMK keymap API.
 *
 * Uses BEHAVIOR_DT_INST_DEFINE(0, ...) directly instead of
 * DT_INST_FOREACH_STATUS_OKAY to avoid a Zephyr 4.1.0 macro
 * expansion bug with custom behavior nodes in keymap overlays.
 */

/*
 * Devicetree compatible string for this behavior.
 * Must be defined BEFORE including any headers because
 * <drivers/behavior.h> and other ZMK headers may use it.
 * Matches compatible = "zmk,behavior-app-layer" in the keymap.
 *
 * Commas in the devicetree compatible become underscores
 * in the C macro (zmk,behavior-app-layer -> zmk_behavior_app_layer).
 */
#define DT_DRV_COMPAT zmk_behavior_app_layer

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/behavior.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * Shared variable defined in app_layer_sync.c.
 * Holds the layer number that the Mac requested via raw HID.
 * Both this file and app_layer_sync.c read/write this value
 * to stay in sync about which app layer should be active.
 */
extern uint8_t active_app_layer;

/*
 * Static flag tracking whether the app layer is currently active.
 * We use a module-level static instead of per-device data because
 * there is only ever one instance of this behavior.
 */
static bool app_layer_is_active;

/*
 * Behavior keymap binding handler — triggered on key press.
 *
 * When the user presses the &app_layer key, this toggles the
 * application-specific layer on or off:
 *   - If currently active: deactivate active_app_layer
 *   - If currently inactive: activate active_app_layer
 *
 * Returns ZMK_BEHAVIOR_OPAQUE to tell ZMK we handled the event.
 */
static int behavior_app_layer_pressed(struct zmk_behavior_binding *binding,
                                       struct zmk_behavior_binding_event event)
{
    if (app_layer_is_active) {
        /*
         * App layer was active, deactivate it.
         * zmk_keymap_layer_deactivate() removes the layer
         * from the active layer stack.
         * event.source identifies which half (left/right) triggered it.
         */
        zmk_keymap_layer_deactivate(active_app_layer, event.source);
        app_layer_is_active = false;
    } else {
        /*
         * App layer was inactive, activate it.
         * zmk_keymap_layer_activate() pushes the layer
         * onto the active layer stack.
         * event.source identifies which half (left/right) triggered it.
         */
        zmk_keymap_layer_activate(active_app_layer, event.source);
        app_layer_is_active = true;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

/*
 * Behavior keymap binding handler — triggered on key release.
 *
 * Since this is a toggle behavior (not a hold), the release
 * does nothing. We still must define it to satisfy the
 * behavior_driver_api struct.
 */
static int behavior_app_layer_released(struct zmk_behavior_binding *binding,
                                        struct zmk_behavior_binding_event event)
{
    return ZMK_BEHAVIOR_OPAQUE;
}

/*
 * ZMK behavior driver API.
 * Maps the press and release handlers so ZMK can call them
 * when the keymap binding &app_layer is triggered.
 * struct behavior_driver_api is defined in <drivers/behavior.h>.
 */
static const struct behavior_driver_api behavior_app_layer_api = {
    .binding_pressed  = behavior_app_layer_pressed,
    .binding_released = behavior_app_layer_released,
};

/*
 * Device initialization function.
 * Called once at POST_KERNEL priority during boot.
 * Sets the initial state to inactive.
 */
static int behavior_app_layer_init(const struct device *dev)
{
    app_layer_is_active = false;
    return 0;
}

/*
 * Register the behavior device instance.
 *
 * WHY NOT DT_INST_FOREACH_STATUS_OKAY:
 *   In Zephyr 4.1.0, DT_INST_FOREACH_STATUS_OKAY fails to
 *   properly expand for custom behavior nodes declared in
 *   keymap overlay files. The macro produces a literal 'n'
 *   token instead of instance number 0.
 *
 * WORKAROUND:
 *   We call BEHAVIOR_DT_INST_DEFINE(0, ...) directly with
 *   instance number 0, wrapped in a DT_HAS_COMPAT_STATUS_OKAY
 *   guard so the code is only compiled when the app_layer node
 *   exists with status "okay" in the devicetree.
 *
 * BEHAVIOR_DT_INST_DEFINE is a ZMK-specific macro (defined in
 * <drivers/behavior.h>) that wraps Zephyr's DEVICE_DT_INST_DEFINE
 * with ZMK-specific behavior setup.
 */
#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

BEHAVIOR_DT_INST_DEFINE(
    0,                              /* Instance number */
    behavior_app_layer_init,        /* Init function called at boot */
    NULL,                           /* Power management (none) */
    NULL,                           /* Behavior data (none, using static) */
    NULL,                           /* Behavior config (none needed) */
    POST_KERNEL,                    /* Init level */
    CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, /* Priority within level */
    &behavior_app_layer_api);       /* Driver API with press/release handlers */

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */