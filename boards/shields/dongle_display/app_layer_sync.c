/*
 * app_layer_sync.c
 * 
 * Central module for bidirectional synchronization between Selenite (Mac) and the ZMK keyboard.
 * 
 * TWO COMMUNICATION DIRECTIONS:
 * ─────────────────────────────
 *   Mac ──→ Keyboard : Raw HID output report (received via raw_hid_received_event)
 *                     byte[0] = target layer, byte[1] = auto, byte[2] = one-shot, byte[3..] = app name
 *                     Handled by: on_raw_hid_received()
 * 
 *   Keyboard ──→ Mac : Raw HID input report (sent via raise_raw_hid_sent_event)
 *                     byte[0] = currently active layer on the keyboard
 *                     Triggered by: any layer change (zmk_layer_state_changed)
 *                     Handled by: send_active_layer_to_mac()
 * 
 * LAYER STATE ARCHITECTURE:
 * ─────────────────────────
 *   current_app_layer  : The layer the Mac has requested (stored in memory).
 *                        Can be 0 (base) or a layer number.
 *                        This layer is NOT necessarily physically active.
 * 
 *   active_app_layer   : The layer that is ACTUALLY active on the keyboard.
 *                        0 when no app layer is activated.
 *                        Only this layer is visible to the user.
 *                        Shared with behavior_app_layer.c (extern).
 * 
 *   is_layer_persistent : If false, the layer deactivates after ONE keypress (one-shot).
 *                         If true, the layer stays active indefinitely until manual action.
 * 
 * OPERATING MODES:
 * ─────────────────
 *   Auto Mode     : The layer is activated IMMEDIATELY upon receiving the Mac's message.
 *                   No need to press &app_layer.
 * 
 *   Manual Mode   : The layer is only STORED in memory.
 *                   The user must press &app_layer to activate/deactivate it.
 * 
 *   One-shot Mode : The layer deactivates automatically after the next keypress
 *                   (except modifiers: Cmd, Shift, Alt, Ctrl).
 *                   Compatible with both Auto and Manual modes.
 */

// Zephyr kernel (k_sem, threading, etc.)
#include <zephyr/kernel.h>

// ZMK event manager (ZMK_LISTENER, ZMK_SUBSCRIPTION, ZMK_EV_EVENT_BUBBLE, etc.)
#include <zmk/event_manager.h>

// Keycode event struct and casting (as_zmk_keycode_state_changed)
// Used in Section 3 for F13 toggle and one-shot deactivation
#include <zmk/events/keycode_state_changed.h>

// Layer state changed event (zmk_layer_state_changed)
// Used in Section 1 to detect when any layer changes on the keyboard
#include <zmk/events/layer_state_changed.h>

// zmk_keymap_layer_activate/deactivate/active, zmk_keymap_highest_layer_active
#include <zmk/keymap.h>

// ZMK HID definitions (not directly used but included for HID constants)
#include <zmk/hid.h>

// Raw HID event types from zmk-raw-hid module:
//   struct raw_hid_received_event — Mac -> Keyboard (we read this)
//   struct raw_hid_sent_event — Keyboard -> Mac (we raise this)
//   raise_raw_hid_sent_event() — send an input report to the Mac
//   as_raw_hid_received_event() — cast helper for received events
#include <raw_hid/events.h>


/* ==========================================================================
 * GLOBAL VARIABLES
 * ========================================================================== */

// The layer the Mac has requested for the current app.
// Stays in memory even when not physically active (Manual mode).
// Examples: 0 = base (no app), 3 = Figma layer, 7 = Autocad layer
// Read by behavior_app_layer.c to know which layer to toggle.
uint8_t current_app_layer = 0;

// The layer that is ACTUALLY active on the keyboard right now.
// 0 = no app layer active (user is on the base layer).
// >0 = an app layer is currently overlaid on top of the base.
// Updated by: this file AND behavior_app_layer.c (extern shared).
uint8_t active_app_layer = 0;

// Whether the active layer should remain after a keypress.
// false = one-shot mode: single keypress then return to base.
// true  = persistent mode: layer stays until explicitly deactivated.
bool is_layer_persistent = false;


/* ==========================================================================
 * SECTION 1 : KEYBOARD → MAC  (Send active layer to Selenite)
 * ========================================================================== */

/*
 * send_active_layer_to_mac()
 * 
 * Sends a 32-byte Raw HID input report to the Mac with the currently active layer.
 * 
 * PROTOCOL:
 *   byte[0]      = highest active layer number (0 = base)
 *   byte[1..31]  = reserved (set to 0)
 * 
 * The Mac (HIDManager.swift) listens for this report via
 * IOHIDManagerRegisterInputReportCallback and updates the overlay
 * (bubble + menu bar) with the real active layer.
 * 
 * IMPLEMENTATION:
 *   Uses raise_raw_hid_sent_event() from the zmk-raw-hid module.
 *   The module's internal USB listener catches this event and writes
 *   the report to the USB HID endpoint. If the dongle is not connected
 *   to the Mac, the send fails silently (no crash).
 * 
 * CALLED BY: on_layer_state_changed() — invoked on EVERY layer change.
 */
static void send_active_layer_to_mac(void) {
    // 32-byte report buffer, initialized to all zeros
    uint8_t report[32] = {0};

    // zmk_keymap_highest_layer_active() returns the index of the
    // highest-numbered layer that is currently active.
    // 0 = base, 1 = first layer, 2 = second layer, etc.
    // This is the standard ZMK API for "what layer am I on right now".
    report[0] = zmk_keymap_highest_layer_active();

    // Raise the raw_hid_sent_event so the zmk-raw-hid module's
    // USB listener (usb_hid.c) picks it up and sends it over USB.
    // The .data field points to our local report buffer.
    // The .length field tells the module how many bytes to send.
    raise_raw_hid_sent_event((struct raw_hid_sent_event){
        .data = report,
        .length = sizeof(report)
    });
}

/*
 * on_layer_state_changed()
 * 
 * ZMK event listener triggered on EVERY layer change on the keyboard.
 * 
 * This captures ALL sources of layer changes, including:
 *   - Activation/deactivation by this module (auto, F13, one-shot)
 *   - Activation by &app_layer behavior (behavior_app_layer.c)
 *   - Activation by MO (momentary layer) keys in the keymap
 *   - Activation by TO (toggle layer) keys in the keymap
 *   - Any other ZMK mechanism that changes the active layer
 * 
 * On every change, we inform the Mac of the NEW active layer.
 * The Mac compares this number with its assumption and updates the overlay.
 * 
 * ZMK_EV_EVENT_BUBBLE: We let the event continue to other listeners.
 * This is critical — the layer_status.c widget (dongle OLED display)
 * also subscribes to zmk_layer_state_changed and needs this event
 * to update the layer name on screen.
 */
static int on_layer_state_changed(const zmk_event_t *eh) {
    // Send the new active layer number to the Mac
    send_active_layer_to_mac();

    // Let the event propagate to other ZMK listeners
    // (layer_status widget, dongle display, etc.)
    return ZMK_EV_EVENT_BUBBLE;
}

// Register the listener with ZMK's event system.
// "app_layer_report" is the unique identifier for this listener.
ZMK_LISTENER(app_layer_report, on_layer_state_changed);

// Subscribe to layer state changes.
// zmk_layer_state_changed is emitted by ZMK every time a layer
// is activated or deactivated, regardless of the source.
ZMK_SUBSCRIPTION(app_layer_report, zmk_layer_state_changed);


/* ==========================================================================
 * SECTION 2 : MAC → KEYBOARD  (Receive layer commands from Selenite)
 * ========================================================================== */

/*
 * on_raw_hid_received()
 * 
 * ZMK event listener triggered when the Mac sends a 32-byte Raw HID output report.
 * 
 * RECEIVED PROTOCOL:
 *   byte[0]      = target layer number (0 = return to base)
 *   byte[1]      = auto mode (1 = activate immediately, 0 = store in memory)
 *   byte[2]      = one-shot mode (1 = deactivate after one keypress, 0 = persistent)
 *   byte[3..31]  = application name (UTF-8 text, informational only, not used here)
 * 
 * PROCESSING LOGIC:
 *   1. Deactivate the previous app layer if it was active
 *   2. Based on the mode (auto/manual):
 *      - Auto: activate the new layer immediately
 *      - Manual: only store it, user activates via &app_layer
 *   3. Inform the Mac of the actually active layer (via send_active_layer_to_mac)
 * 
 * IMPORTANT: We always call send_active_layer_to_mac() at the end, even for
 * layer==0 where no ZMK layer change occurs. This ensures the Mac always
 * gets a response, even if it's just "0" (base layer).
 */
static int on_raw_hid_received(const zmk_event_t *eh) {
    // Cast the generic event to the specific raw_hid_received_event type.
    // Returns NULL if the event is not a raw HID received event.
    const struct raw_hid_received_event *ev = as_raw_hid_received_event(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    
    // --- Decode the Selenite protocol ---
    // Target layer number requested by the Mac (0 = base, 7 = Autocad, etc.)
    uint8_t layer = ev->data[0];
    // Auto mode: should we activate the layer immediately?
    bool is_auto = (ev->data[1] == 1);
    // One-shot mode: should we deactivate after one keypress?
    bool is_one_shot = (ev->data[2] == 1);
    
    // --- Step 1: Clean up the previous app layer ---
    // If an app layer was previously active, deactivate it BEFORE
    // activating a new one. This prevents layer stacking.
    // The "true" parameter forces deactivation even if the layer was
    // activated by a standard ZMK mechanism (MO, TO, etc.)
    if (active_app_layer > 0 && zmk_keymap_layer_active(active_app_layer)) {
        zmk_keymap_layer_deactivate(active_app_layer, true);
    }
    
    // Reset state variables
    active_app_layer = 0;        // No app layer is active anymore
    is_layer_persistent = false;  // Reset persistence flag
    
    // --- Step 2: Process the new layer command ---
    if (layer == 0) {
        // ──────────────────────────────────────────────
        // CASE A: Return to base (no app-specific layer)
        // ──────────────────────────────────────────────
        // The Mac says "no app layer to activate".
        // Reset current_app_layer to 0.
        // The active layer falls back to 0 (base) via the deactivation above.
        current_app_layer = 0;
        
    } else if (is_auto) {
        // ──────────────────────────────────────────────
        // CASE B: Auto mode — immediate activation
        // ──────────────────────────────────────────────
        // The Mac requests the layer to be activated DIRECTLY, no user action needed.
        // Typically used when the user focuses an app with "Auto" checked in Selenite.
        // This triggers zmk_layer_state_changed, which calls send_active_layer_to_mac.
        
        // Activate the layer immediately
        zmk_keymap_layer_activate(layer, true);
        // Record that this layer is now physically active
        active_app_layer = layer;
        // Record the layer requested by the Mac
        current_app_layer = layer;
        // Persistent unless one-shot is enabled
        is_layer_persistent = !is_one_shot;
        
    } else {
        // ──────────────────────────────────────────────
        // CASE C: Manual mode — store in memory only
        // ──────────────────────────────────────────────
        // The Mac says "here's the layer for this app" but we do NOT activate it.
        // The user must press &app_layer to activate it manually.
        // Useful for apps where you don't want automatic layer switching.
        
        // Store the layer number for later activation by &app_layer
        current_app_layer = layer;
        // Persistent unless one-shot is enabled
        is_layer_persistent = !is_one_shot;
    }
    
    // --- Step 3: Inform the Mac of the result ---
    // After processing the command, we send the ACTUALLY active layer to the Mac.
    // In auto mode, the Mac will receive the new layer number.
    // In manual mode, the Mac will receive 0 (base) because we didn't activate anything.
    //
    // NOTE: on_layer_state_changed will also be triggered by the
    // zmk_keymap_layer_activate/deactivate calls above, which will also
    // call send_active_layer_to_mac(). But we call it here explicitly for
    // the layer==0 case where no ZMK layer change occurs, ensuring the
    // Mac always gets a response to its command.
    send_active_layer_to_mac();
    
    return ZMK_EV_EVENT_BUBBLE;
}


/* ==========================================================================
 * SECTION 3 : KEYBOARD INPUT HANDLING  (F13 toggle + One-shot deactivation)
 * ========================================================================== */

/*
 * on_keycode_state_changed()
 * 
 * ZMK event listener triggered on EVERY key press or release on the keyboard.
 * 
 * TWO FUNCTIONS:
 *   1. F13 TOGGLE: If the user presses F13, activate/deactivate the layer
 *      stored in current_app_layer (Manual mode).
 * 
 *   2. ONE-SHOT: If the active layer is in non-persistent mode (one-shot)
 *      and the user presses an ORDINARY key (not a modifier), deactivate
 *      the layer after this keypress.
 * 
 * F13 is an alternative to &app_layer. Both achieve the same toggle.
 * &app_layer uses behavior_app_layer.c, F13 uses this listener.
 * They are mutually exclusive in the keymap (use one or the other).
 * 
 * HID MODIFIER KEY CODES (usage page 0x07):
 *   0xE0 = Left Ctrl,  0xE1 = Left Shift,  0xE2 = Left Alt,  0xE3 = Left GUI (Cmd)
 *   0xE4 = Right Ctrl, 0xE5 = Right Shift, 0xE6 = Right Alt, 0xE7 = Right GUI
 */
static int on_keycode_state_changed(const zmk_event_t *eh) {
    // Cast the generic event to a keycode state changed event.
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    
    // Ignore key releases and invalid events.
    // We only react to key PRESSES (state == true).
    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    // ──────────────────────────────────────────
    // FUNCTION 1: TOGGLE via F13 (keycode 0x68)
    // ──────────────────────────────────────────
    // F13 is the key mapped in the keymap to toggle the current app layer.
    // Usage page 0x07 = Keyboard/Keypad, keycode 0x68 = F13.
    if (ev->usage_page == 0x07 && ev->keycode == 0x68) {
        if (current_app_layer > 0 && active_app_layer == 0) {
            // No layer is active but a layer is stored in memory -> ACTIVATE it
            // This triggers zmk_layer_state_changed -> send_active_layer_to_mac
            zmk_keymap_layer_activate(current_app_layer, true);
            active_app_layer = current_app_layer;
        } else if (active_app_layer > 0) {
            // A layer is active -> DEACTIVATE it (return to base)
            // This triggers zmk_layer_state_changed -> send_active_layer_to_mac
            zmk_keymap_layer_deactivate(active_app_layer, true);
            active_app_layer = 0;
        }
        // ZMK_EV_EVENT_HANDLED: We consume the F13 event to prevent it
        // from being processed as a normal keypress by ZMK.
        // Without this, F13 would be sent to the Mac as a regular keystroke.
        return ZMK_EV_EVENT_HANDLED;
    }

    // ──────────────────────────────────────────
    // FUNCTION 2: ONE-SHOT (deactivate after 1 keypress)
    // ──────────────────────────────────────────
    // If an app layer is active AND it's in non-persistent mode (one-shot),
    // deactivate it after the next ordinary keypress.
    if (active_app_layer > 0 && !is_layer_persistent) {
        
        // Check if the pressed key is a modifier (Ctrl, Shift, Alt, Cmd/GUI).
        // Modifiers do NOT trigger one-shot deactivation.
        // This allows: Activate layer -> Hold Cmd -> Type a shortcut
        // without the layer deactivating too early.
        bool is_mod = (ev->usage_page == 0x07 && ev->keycode >= 0xE0 && ev->keycode <= 0xE7);
        
        if (!is_mod) {
            // Ordinary key pressed: deactivate the one-shot layer
            // This triggers zmk_layer_state_changed -> send_active_layer_to_mac
            zmk_keymap_layer_deactivate(active_app_layer, true);
            active_app_layer = 0;
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}


/* ==========================================================================
 * ZMK EVENT LISTENER REGISTRATION
 * ========================================================================== */

// Listener 1: Receive Raw HID commands from the Mac.
// Identifier: "app_layer_sync_hid"
// Subscribed to: raw_hid_received_event (emitted by the zmk-raw-hid module
// when the dongle receives a USB output report from the Mac).
ZMK_LISTENER(app_layer_sync_hid, on_raw_hid_received);
ZMK_SUBSCRIPTION(app_layer_sync_hid, raw_hid_received_event);

// Listener 2: Detect physical key presses (F13 toggle + one-shot).
// Identifier: "app_layer_sync_key"
// Subscribed to: zmk_keycode_state_changed (emitted by ZMK on every
// key press or release across the entire keyboard).
ZMK_LISTENER(app_layer_sync_key, on_keycode_state_changed);
ZMK_SUBSCRIPTION(app_layer_sync_key, zmk_keycode_state_changed);