/*
 * behavior_app_layer.c
 *
 * Custom ZMK behavior that toggles application-specific layers.
 * Bound in the keymap with &app_layer on a thumb key.
 *
 * ARCHITECTURE OVERVIEW:
 * ─────────────────────
 * The keyboard uses a two-variable layer state system:
 *
 *   current_app_layer  — The layer number the Mac REQUESTED via Raw HID.
 *                       Stored in memory but NOT necessarily active on
 *                       the keyboard. Updated only by app_layer_sync.c
 *                       when the Mac sends a new layer command.
 *                       Examples: 0 = no app, 7 = Autocad, 8 = Word, etc.
 *
 *   active_app_layer   — The layer number ACTIVELY running on the keyboard.
 *                       0 = base layer (no app overlay active).
 *                       >0 = an app layer is physically overlaid on base.
 *                       Shared between this file and app_layer_sync.c.
 *
 * HOW IT WORKS (Manual Mode):
 * ─────────────────────
 *   1. Mac focuses Word → sends layer=8, auto=0 via Raw HID
 *   2. app_layer_sync.c stores current_app_layer=8, active_app_layer stays 0
 *   3. User presses &app_layer
 *   4. This behavior detects current_app_layer>0 && active_app_layer==0
 *      → activates layer 8, sets active_app_layer=8
 *   5. User presses &app_layer again
 *   6. This behavior detects active_app_layer>0
 *      → deactivates layer 8, sets active_app_layer=0
 *      → current_app_layer stays 8 (preserved for re-activation)
 *
 * HOW IT WORKS (Auto Mode):
 * ─────────────────────
 *   Auto layers (e.g. Calculator, layer 10) are activated immediately
 *   by app_layer_sync.c when the Mac sends the command. This behavior
 *   is NOT involved in auto activation, but CAN be used to manually
 *   deactivate an auto layer (active_app_layer > 0 path).
 *
 * COMPATIBILITY NOTE:
 * ─────────────────────
 *   Uses BEHAVIOR_DT_INST_DEFINE(0, ...) directly instead of
 *   DT_INST_FOREACH_STATUS_OKAY to avoid a Zephyr 4.1.0 macro
 *   expansion bug with custom behavior nodes in keymap overlays.
 *   The macro produces a literal 'n' token instead of instance 0.
 *   Workaround: hardcode instance 0 with a DT_HAS_COMPAT_STATUS_OKAY guard.
 */

/*
 * Devicetree compatible string for this behavior.
 *
 * MUST be defined BEFORE including any headers because
 * <drivers/behavior.h> and other ZMK headers may reference it.
 *
 * Matches the devicetree node in the keymap:
 *   compatible = "zmk,behavior-app-layer";
 *
 * Naming convention: commas in the devicetree compatible string
 * become underscores in the C macro:
 *   zmk,behavior-app-layer → zmk_behavior_app_layer
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

/* =========================================================================
 * SHARED VARIABLES (defined in app_layer_sync.c, declared extern here)
 * =========================================================================
 *
 * These two variables form the core of the two-state layer management system.
 * They are defined (not extern) in app_layer_sync.c and shared across both
 * this file and the event listeners in app_layer_sync.c.
 */

/*
 * current_app_layer — The layer the Mac has requested for the current app.
 *
 * Updated ONLY by app_layer_sync.c when a Raw HID message arrives from Selenite.
 * This value persists even when no app layer is physically active on the
 * keyboard (Manual mode). It represents "what the Mac wants", not "what is
 * currently running".
 *
 * Read by this behavior to know WHICH layer to activate on toggle.
 * Examples: 0 = no app, 3 = Figma, 7 = Autocad, 8 = Word, 9 = Excel, 10 = Calc.
 */
extern uint8_t current_app_layer;

/*
 * active_app_layer — The layer ACTIVELY running on the keyboard right now.
 *
 * Written by BOTH this file (behavior_app_layer.c) and app_layer_sync.c.
 *   - app_layer_sync.c: updates it on Raw HID receive (auto mode), F13 toggle,
 *                       and one-shot deactivation.
 *   - This file: updates it on &app_layer key press.
 *
 * 0 = base layer only (no app overlay active).
 * >0 = an app-specific layer is currently overlaid on the base.
 *
 * Read by the dongle OLED display (layer_status.c) to show the active layer number.
 * Read by app_layer_sync.c to decide whether to activate or deactivate.
 */
extern uint8_t active_app_layer;

/*
 * app_layer_is_active — Local boolean mirror of active_app_layer state.
 *
 * Tracks whether an app layer is currently active. Redundant with
 * active_app_layer > 0 but kept for explicit boolean semantics.
 * Set during init, updated in the press handler.
 */
static bool app_layer_is_active;

/* =========================================================================
 * BEHAVIOR HANDLERS
 * =========================================================================
 */

/*
 * behavior_app_layer_pressed() — Toggle handler, called on key press.
 *
 * This is the core logic. When the user presses the physical key bound
 * to &app_layer in the keymap, ZMK calls this function.
 *
 * DECISION TREE:
 * ─────────────
 *
 *   Path 1: ACTIVATION
 *     Condition: current_app_layer > 0 && active_app_layer == 0
 *     Meaning:  The Mac has stored a layer number (Manual mode) but it's
 *               not yet physically active on the keyboard.
 *     Action:   Activate current_app_layer in the ZMK keymap, then
 *               synchronize active_app_layer so both modules agree.
 *     Use case: Mac sent layer=8 (Word), user presses &app_layer to switch.
 *
 *   Path 2: DEACTIVATION
 *     Condition: active_app_layer > 0
 *     Meaning:  An app layer is currently active on the keyboard.
 *     Action:   Deactivate it in the ZMK keymap, reset active_app_layer to 0.
 *               current_app_layer is PRESERVED — the Mac's request stays in
 *               memory so the user can re-press &app_layer to re-activate.
 *     Use case: User presses &app_layer again to go back to base.
 *
 *   Path 3: NO-OP
 *     Condition: current_app_layer == 0 && active_app_layer == 0
 *     Meaning:  No app layer requested and none active. Nothing to toggle.
 *     Action:   Return immediately (implicit — both conditions are false).
 *     Use case: Base layer with no app focused on the Mac.
 *
 * OWNERSHIP TRACKING:
 * ─────────────────
 *   We pass event.source (not true) to zmk_keymap_layer_activate/deactivate.
 *   This ensures ZMK's internal source tracking associates the layer change
 *   with the physical key that triggered it, which is important for proper
 *   cleanup if the key is held and the dongle disconnects.
 *
 * Returns ZMK_BEHAVIOR_OPAQUE to tell ZMK we fully handled the event
 * and no further processing (keycode generation, etc.) is needed.
 */
static int behavior_app_layer_pressed(struct zmk_behavior_binding *binding,
                                       struct zmk_behavior_binding_event event)
{
    if (current_app_layer > 0 && active_app_layer == 0) {
        /*
         * ACTIVATION (Manual mode toggle-on):
         *
         * The Mac has stored a layer number in current_app_layer via
         * Raw HID (Manual mode: auto=0), but it has not been activated
         * yet (active_app_layer is still 0).
         *
         * We activate the layer in ZMK's keymap using current_app_layer
         * as the target, then copy it into active_app_layer to keep
         * the two-variable state synchronized.
         *
         * After this, app_layer_sync.c's on_layer_state_changed listener
         * fires (via zmk_layer_state_changed event) and sends the new
         * active layer number to the Mac via Raw HID input report.
         */
        zmk_keymap_layer_activate(current_app_layer, event.source);
        active_app_layer = current_app_layer;
        app_layer_is_active = true;

    } else if (active_app_layer > 0) {
        /*
         * DEACTIVATION (toggle-off):
         *
         * An app layer is currently active on the keyboard. The user
         * wants to return to the base layer.
         *
         * We deactivate the active layer in ZMK's keymap, then reset
         * active_app_layer to 0 (base layer).
         *
         * IMPORTANT: current_app_layer is NOT reset. It stays at the
         * value the Mac set (e.g. 8 for Word). This allows the user
         * to press &app_layer again to re-activate the same layer
         * without needing the Mac to resend anything.
         *
         * If the user switches to a different app on the Mac, Selenite
         * will send a new Raw HID command which updates current_app_layer.
         */
        zmk_keymap_layer_deactivate(active_app_layer, event.source);
        active_app_layer = 0;
        app_layer_is_active = false;
    }
    /*
     * NO-OP case (implicit): current_app_layer == 0 && active_app_layer == 0
     * No app layer requested by the Mac and none active. The &app_layer
     * press is silently ignored. This can happen when the user presses
     * &app_layer but no app-specific layer has been configured for the
     * currently focused application.
     */

    return ZMK_BEHAVIOR_OPAQUE;
}

/*
 * behavior_app_layer_released() — Release handler, called on key release.
 *
 * This is a TOGGLE behavior, not a hold/layer-tap. The layer state change
 * happens entirely in the press handler. On release, we do nothing.
 *
 * We must still define this function to satisfy the struct behavior_driver_api
 * requirement — ZMK calls it via the .binding_released function pointer
 * and expects a valid return code.
 *
 * Returns ZMK_BEHAVIOR_OPAQUE to indicate we handled (ignored) the release.
 */
static int behavior_app_layer_released(struct zmk_behavior_binding *binding,
                                        struct zmk_behavior_binding_event event)
{
    return ZMK_BEHAVIOR_OPAQUE;
}

/* =========================================================================
 * ZMK BEHAVIOR DRIVER API
 * =========================================================================
 */

/*
 * behavior_app_layer_api — Driver API struct that maps ZMK events to handlers.
 *
 * ZMK uses this struct to know which functions to call when the keymap
 * binding &app_layer is triggered by a physical key press or release.
 *
 * struct behavior_driver_api is defined in <zephyr/drivers/behavior.h>.
 * It requires at minimum:
 *   .binding_pressed  — called on key down (press)
 *   .binding_released — called on key up (release)
 *
 * Optional members (not used here):
 *   .locality_trigger     — for position-dependent behaviors
 *   .sensor_binding_triggered — for sensor-based behaviors
 */
static const struct behavior_driver_api behavior_app_layer_api = {
    .binding_pressed  = behavior_app_layer_pressed,
    .binding_released = behavior_app_layer_released,
};

/* =========================================================================
 * DEVICE INITIALIZATION
 * =========================================================================
 */

/*
 * behavior_app_layer_init() — Called once at boot during POST_KERNEL phase.
 *
 * Sets the initial state: no app layer is active at startup.
 * The user must press &app_layer or the Mac must send an auto layer
 * command before any app layer becomes active.
 *
 * Returns 0 on success (required by Zephyr init function signature).
 */
static int behavior_app_layer_init(const struct device *dev)
{
    app_layer_is_active = false;
    return 0;
}

/* =========================================================================
 * DEVICE INSTANCE REGISTRATION
 * =========================================================================
 */

/*
 * Register the behavior device instance with Zephyr/ZMK.
 *
 * WHY NOT DT_INST_FOREACH_STATUS_OKAY:
 * ───────────────────────────────────
 *   In Zephyr 4.1.0, DT_INST_FOREACH_STATUS_OKAY fails to properly expand
 *   for custom behavior nodes declared in keymap overlay files (.overlay).
 *   The macro produces a literal 'n' token instead of the instance number 0,
 *   causing a compilation error about an undeclared identifier.
 *
 * WORKAROUND:
 * ──────────
 *   We call BEHAVIOR_DT_INST_DEFINE(0, ...) directly with instance number 0,
 *   wrapped in a DT_HAS_COMPAT_STATUS_OKAY guard. This ensures the code is
 *   only compiled when the app_layer node exists with status "okay" in the
 *   devicetree (defined in the keymap .keymap file).
 *
 * BEHAVIOR_DT_INST_DEFINE is a ZMK-specific macro (defined in
 * <zmk/drivers/behavior.h>) that wraps Zephyr's DEVICE_DT_INST_DEFINE with
 * ZMK-specific behavior setup (event manager integration, etc.).
 *
 * Parameters:
 *   0                               — Instance number (first and only instance)
 *   behavior_app_layer_init          — Init function (called at boot)
 *   NULL                             — PM device (no power management)
 *   NULL                             — Behavior data (no per-instance data needed)
 *   NULL                             — Behavior config (no devicetree config)
 *   POST_KERNEL                      — Init level (runs after kernel services)
 *   CONFIG_KERNEL_INIT_PRIORITY_DEFAULT — Priority within POST_KERNEL level
 *   &behavior_app_layer_api          — Driver API with press/release handlers
 */
#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

BEHAVIOR_DT_INST_DEFINE(
    0,
    behavior_app_layer_init,
    NULL,
    NULL,
    NULL,
    POST_KERNEL,
    CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
    &behavior_app_layer_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */