/*
 * behavior_app_layer.c
 * 
 * Custom ZMK behavior: &app_layer
 * 
 * Toggle behavior for the current app layer.
 * Used in the keymap as &app_layer on a thumb key.
 * Press once to activate the layer, press again to deactivate.
 * 
 * IMPORTANT: This file shares state with app_layer_sync.c.
 * Both files read/write current_app_layer and active_app_layer.
 * Keeping them in sync is critical for correct operation.
 * 
 * When this behavior toggles a layer, zmk_layer_state_changed is fired,
 * which triggers app_layer_sync.c to send the new active layer to the Mac.
 */

#include <zephyr/device.h>

// ZMK behavior driver API (binding_pressed, binding_released)
#include <drivers/behavior.h>

// ZMK behavior utilities (ZMK_BEHAVIOR_OPAQUE, etc.)
#include <zmk/behavior.h>

// zmk_keymap_layer_active(), zmk_keymap_layer_activate/deactivate()
#include <zmk/keymap.h>

// ZMK event manager (required for behavior drivers)
#include <zmk/event_manager.h>

// Matches the compatible string in zmk,behavior-app-layer.yaml
#define DT_DRV_COMPAT zmk_behavior_app_layer

// =============================================================================
// SHARED STATE (defined in app_layer_sync.c)
// =============================================================================

// The layer number the Mac has requested for the current app.
// 0 = no app with a layer rule is focused.
// >0 = a target layer is stored in memory.
extern uint8_t current_app_layer;

// The layer that is ACTUALLY active on the keyboard right now.
// 0 = no app layer is active (base layer).
// >0 = an app layer is currently overlaid on top of the base.
// We update this variable to keep app_layer_sync.c in sync.
extern uint8_t active_app_layer;

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

// =============================================================================
// INITIALIZATION
// =============================================================================

// Behavior driver init function. Required by ZMK but nothing to initialize.
static int behavior_app_layer_init(const struct device *dev) { return 0; };

// =============================================================================
// TOGGLE BEHAVIOR (press to activate, press again to deactivate)
// =============================================================================

// Called when the user PRESSES the &app_layer key.
//
// Logic:
//   - If current_app_layer is 0 (no app with a layer rule focused):
//       Do nothing. Return ZMK_BEHAVIOR_OPAQUE (key consumed, no action).
//
//   - If the layer is currently active (zmk_keymap_layer_active returns true):
//       Deactivate it and set active_app_layer = 0.
//       This fires zmk_layer_state_changed -> app_layer_sync sends 0 to Mac.
//
//   - If the layer is NOT currently active:
//       Activate it and set active_app_layer = current_app_layer.
//       This fires zmk_layer_state_changed -> app_layer_sync sends the layer to Mac.
//
// NOTE: We update active_app_layer here so app_layer_sync.c's state stays
// consistent. Without this, app_layer_sync.c would think no layer is active
// even though we just activated one, causing desync.
static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    // Only act if the Mac has assigned a layer for the current app
    if (current_app_layer > 0) {
        if (zmk_keymap_layer_active(current_app_layer)) {
            // Layer is currently active -> deactivate it (return to base)
            zmk_keymap_layer_deactivate(current_app_layer, true);
            // Keep app_layer_sync.c in sync
            active_app_layer = 0;
        } else {
            // Layer is not active -> activate it
            zmk_keymap_layer_activate(current_app_layer, true);
            // Keep app_layer_sync.c in sync
            active_app_layer = current_app_layer;
        }
    }
    // ZMK_BEHAVIOR_OPAQUE: key is consumed, no further processing needed
    return ZMK_BEHAVIOR_OPAQUE;
}

// Called when the user RELEASES the &app_layer key.
// Nothing to do — this is a toggle, not a momentary layer.
static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

// =============================================================================
// DRIVER API TABLE (required by ZMK for all behavior drivers)
// =============================================================================

// Maps the ZMK behavior API to our functions above.
// ZMK calls binding_pressed/binding_released through this table.
static const struct behavior_driver_api behavior_app_layer_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

// =============================================================================
// DEVICE INSTANTIATION (standard ZMK pattern for all drivers)
// =============================================================================

// Creates a device instance for each devicetree node with
// compatible = "zmk,behavior-app-layer" and status = "okay".
// DT_INST_FOREACH_STATUS_OKAY iterates over all matching nodes.
#define KP_INST(n)
DEVICE_DT_INST_DEFINE(n, behavior_app_layer_init, NULL, NULL, NULL, POST_KERNEL,
CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_app_layer_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KP_INST)

#endif
