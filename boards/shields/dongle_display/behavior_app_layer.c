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
 * We intentionally avoid DT_INST_FOREACH_STATUS_OKAY because Zephyr 4.1.0
 * fails to expand it correctly for custom behavior nodes declared in
 * keymap overlay files. Instead we use DEVICE_DT_DEFINE with DT_NODELABEL
 * which resolves the node directly by its label.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zmk/behavior.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/keymap.h>

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
 * Devicetree compatible string for this behavior.
 * Must match the compatible property in the keymap node:
 *   compatible = "zmk,behavior-app-layer";
 *
 * Commas in the devicetree compatible become underscores
 * in the C macro (zmk,behavior-app-layer -> zmk_behavior_app_layer).
 */
#define DT_DRV_COMPAT zmk_behavior_app_layer

/*
 * Define the device instance directly using the node label.
 *
 * WHY NOT DT_INST_FOREACH_STATUS_OKAY:
 *   In Zephyr 4.1.0, DT_INST_FOREACH_STATUS_OKAY can fail to
 *   properly expand for custom behavior nodes declared in keymap
 *   overlay files. The macro ends up with a literal 'n' token
 *   instead of the instance number, causing:
 *     DT_N_INST_n_zmk_behavior_app_layer_FULL_NAME undeclared
 *
 * WORKAROUND:
 *   We use DEVICE_DT_DEFINE with DT_NODELABEL(app_layer) to
 *   reference the node directly by its label, bypassing the
 *   DT_INST instance numbering system entirely.
 *
 * The guard DT_HAS_COMPAT_STATUS_OKAY ensures this code is only
 * compiled when the app_layer node exists and has status "okay"
 * in the devicetree (i.e., when building the dongle firmware
 * which includes the keymap with this node).
 */
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_app_layer)

/*
 * Direct device definition using the node label from the keymap.
 * DEVICE_DT_DEFINE parameters:
 *   node_id   - the devicetree node (looked up by label "app_layer")
 *   init_fn   - initialization function called at boot
 *   pm_device - power management (NULL = none)
 *   data_ptr  - per-device data (NULL, we use static variable)
 *   config_ptr- per-device config (NULL, no config needed)
 *   level     - initialization level (POST_KERNEL)
 *   prio      - priority within level (CONFIG_KERNEL_INIT_PRIORITY_DEFAULT)
 *   api_ptr   - driver API struct (our behavior_app_layer_api)
 */
DEVICE_DT_DEFINE(DT_NODELABEL(app_layer),
                 behavior_app_layer_init,
                 NULL,
                 NULL,
                 NULL,
                 POST_KERNEL,
                 CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                 &behavior_app_layer_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_app_layer) */