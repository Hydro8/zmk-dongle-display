#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/split/transport/central.h>
#include <zmk/split/transport/types.h>
#include <zmk/usb.h>

#include <raw_hid/events.h>

#define AEK_REPORT_SIZE 32
#define AEK_HEADER_SIZE 8
#define AEK_MAX_PAYLOAD 24
#define AEK_MAGIC 0xAE
#define AEK_MAJOR 1
#define AEK_MINOR 0
#define AEK_NONE_REF 0xFF

#define AEK_MSG_HELLO 0x01
#define AEK_MSG_GET_STATE 0x02
#define AEK_MSG_SET_LAYER_INTENT 0x10
#define AEK_MSG_STATE_SNAPSHOT 0x80
#define AEK_MSG_HELLO_ACK 0x81
#define AEK_MSG_LINK_STATE_CHANGED 0x82
#define AEK_MSG_PHYSICAL_KEY_STATE 0x83
#define AEK_MSG_ACK 0x90
#define AEK_MSG_NACK 0x91

#define AEK_INTENT_ACTIVATE_NOW BIT(0)
#define AEK_INTENT_ONE_SHOT BIT(1)
#define AEK_INTENT_KNOWN_MASK (AEK_INTENT_ACTIVATE_NOW | AEK_INTENT_ONE_SHOT)

#define AEK_ERR_UNSUPPORTED_VERSION 0x01
#define AEK_ERR_UNSUPPORTED_MESSAGE 0x02
#define AEK_ERR_INVALID_PAYLOAD 0x03
#define AEK_ERR_INVALID_LAYER 0x04
#define AEK_ERR_NOT_READY 0x05
#define AEK_ERR_KEYBOARD_UNAVAILABLE 0x06
#define AEK_ERR_BUSY 0x07
#define AEK_ERR_INTERNAL_ERROR 0x08

#define AEK_CAP_STATE_SNAPSHOT BIT(0)
#define AEK_CAP_LAYER_INTENT BIT(1)
#define AEK_CAP_LEGACY_COMPAT BIT(2)
#define AEK_CAP_PHYSICAL_KEY_STATE BIT(3)

#define AEK_LINK_DISCONNECTED 0
#define AEK_LINK_SOME_CONNECTED 1
#define AEK_LINK_ALL_CONNECTED 2
#define AEK_LINK_POLL_MS 250

#define AEK_POSITION_COUNT 64
#define AEK_POSITION_BITMAP_BYTES 8

struct aek_layer_map {
    const char *layer_id;
    uint8_t wire_ref;
    zmk_keymap_layer_id_t zmk_id;
};

static const struct aek_layer_map layer_map[] = {
    {"base", 0, 0}, {"num-lock", 1, 1}, {"symbols", 2, 2}, {"vim-nav", 3, 3},
    {"nav-num", 4, 4}, {"num-row", 5, 5}, {"fn-media", 6, 6},
    {"app-autocad", 7, 7}, {"app-word", 8, 8}, {"app-excel", 9, 9}, {"app-calc", 10, 10},
};

uint8_t current_app_layer = 0;
uint8_t active_app_layer = 0;
bool is_layer_persistent = false;

extern const struct zmk_split_transport_central *active_transport;

static bool v1_negotiated;
static uint16_t session_id;
static uint16_t state_revision;
static uint32_t host_nonce;
static uint8_t smart_app_selected_ref = AEK_NONE_REF;
static uint8_t keyboard_link_state = AEK_LINK_DISCONNECTED;
static uint8_t physical_key_bitmap[AEK_POSITION_BITMAP_BYTES];
static bool physical_state_valid;
static bool physical_epoch_tainted;
static bool link_poll_started;

static const struct aek_layer_map *map_from_ref(uint8_t ref) {
    for (size_t i = 0; i < ARRAY_SIZE(layer_map); i++) {
        if (layer_map[i].wire_ref == ref) {
            return &layer_map[i];
        }
    }
    return NULL;
}

static uint8_t ref_from_zmk_id(zmk_keymap_layer_id_t id) {
    for (size_t i = 0; i < ARRAY_SIZE(layer_map); i++) {
        if (layer_map[i].zmk_id == id) {
            return layer_map[i].wire_ref;
        }
    }
    return AEK_NONE_REF;
}

static void send_frame(uint8_t type, uint16_t sequence, const uint8_t *payload, uint8_t length) {
    uint8_t report[AEK_REPORT_SIZE] = {0};
    report[0] = AEK_MAGIC;
    report[1] = AEK_MAJOR;
    report[2] = AEK_MINOR;
    report[3] = type;
    report[4] = 0;
    report[5] = length;
    sys_put_le16(sequence, &report[6]);
    if (payload != NULL && length > 0 && length <= AEK_MAX_PAYLOAD) {
        memcpy(&report[AEK_HEADER_SIZE], payload, length);
    }
    raise_raw_hid_sent_event((struct raw_hid_sent_event){.data = report, .length = sizeof(report)});
}

static void send_ack(uint16_t sequence, uint8_t accepted_type) {
    uint8_t payload[1] = {accepted_type};
    send_frame(AEK_MSG_ACK, sequence, payload, sizeof(payload));
}

static void send_nack(uint16_t sequence, uint8_t rejected_type, uint8_t error) {
    uint8_t payload[2] = {rejected_type, error};
    send_frame(AEK_MSG_NACK, sequence, payload, sizeof(payload));
}

static uint8_t read_keyboard_link_state(void) {
    if (active_transport == NULL || active_transport->api == NULL ||
        active_transport->api->get_status == NULL) {
        return AEK_LINK_DISCONNECTED;
    }

    struct zmk_split_transport_status status = active_transport->api->get_status();
    if (!status.available || !status.enabled) {
        return AEK_LINK_DISCONNECTED;
    }

    switch (status.connections) {
    case ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED:
        return AEK_LINK_ALL_CONNECTED;
    case ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_SOME_CONNECTED:
        return AEK_LINK_SOME_CONNECTED;
    case ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED:
    default:
        return AEK_LINK_DISCONNECTED;
    }
}

static bool layer_state_valid(void) {
    return keyboard_link_state == AEK_LINK_ALL_CONNECTED;
}

static void invalidate_physical_state(void) {
    physical_state_valid = false;
    physical_epoch_tainted = true;
    memset(physical_key_bitmap, 0, sizeof(physical_key_bitmap));
}

static void refresh_physical_validity(uint8_t link_state) {
    if (link_state != AEK_LINK_ALL_CONNECTED) {
        invalidate_physical_state();
        return;
    }
    if (!physical_epoch_tainted) {
        physical_state_valid = true;
    }
}

static void send_physical_key_state(uint16_t sequence) {
    uint8_t payload[2 + AEK_POSITION_BITMAP_BYTES] = {0};
    payload[0] = physical_state_valid ? 1 : 0;
    payload[1] = AEK_POSITION_COUNT;
    if (physical_state_valid) {
        memcpy(&payload[2], physical_key_bitmap, sizeof(physical_key_bitmap));
    }
    send_frame(AEK_MSG_PHYSICAL_KEY_STATE, sequence, payload, sizeof(payload));
}

static void build_active_bitmap(uint8_t bitmap[16]) {
    memset(bitmap, 0, 16);
    for (size_t i = 0; i < ARRAY_SIZE(layer_map); i++) {
        const struct aek_layer_map *entry = &layer_map[i];
        if (zmk_keymap_layer_active(entry->zmk_id)) {
            bitmap[entry->wire_ref / 8] |= BIT(entry->wire_ref % 8);
        }
    }
}

static void send_state_snapshot(uint16_t sequence) {
    uint8_t payload[24] = {0};
    bool valid = layer_state_valid();

    state_revision++;
    sys_put_le16(state_revision, &payload[0]);
    payload[2] = valid ? 1 : 0;
    payload[3] = keyboard_link_state;
    payload[4] = AEK_NONE_REF;
    payload[5] = AEK_NONE_REF;
    payload[6] = 16;

    if (valid) {
        uint8_t bitmap[16];
        zmk_keymap_layer_index_t highest_index = zmk_keymap_highest_layer_active();
        zmk_keymap_layer_id_t highest_id = zmk_keymap_layer_index_to_id(highest_index);
        uint8_t highest_ref = ref_from_zmk_id(highest_id);

        build_active_bitmap(bitmap);
        payload[4] = highest_ref;
        payload[5] = smart_app_selected_ref;
        memcpy(&payload[7], bitmap, sizeof(bitmap));
    }

    send_frame(AEK_MSG_STATE_SNAPSHOT, sequence, payload, sizeof(payload));
}

static void send_link_state_changed(void) {
    uint8_t payload[2] = {keyboard_link_state, layer_state_valid() ? 1 : 0};
    send_frame(AEK_MSG_LINK_STATE_CHANGED, 0, payload, sizeof(payload));
}

static void link_poll_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(aek_link_poll_work, link_poll_work_handler);

static void send_hello_ack(uint16_t sequence) {
    uint8_t payload[14] = {0};
    uint16_t capabilities =
        AEK_CAP_STATE_SNAPSHOT | AEK_CAP_LAYER_INTENT | AEK_CAP_PHYSICAL_KEY_STATE;
#if IS_ENABLED(CONFIG_AEKLIPSE_HID_LEGACY_COMPAT)
    capabilities |= AEK_CAP_LEGACY_COMPAT;
#endif
    sys_put_le32(host_nonce, &payload[0]);
    sys_put_le16(session_id, &payload[4]);
    sys_put_le16(capabilities, &payload[6]);
    sys_put_le16(1, &payload[8]);
    sys_put_le32(0x53454C31u, &payload[10]);
    send_frame(AEK_MSG_HELLO_ACK, sequence, payload, sizeof(payload));
}

static void clear_smart_app_owned_layer(void) {
    if (active_app_layer > 0 && zmk_keymap_layer_active(active_app_layer)) {
        zmk_keymap_layer_deactivate(active_app_layer, false);
    }
    active_app_layer = 0;
    is_layer_persistent = false;
}

static int apply_layer_intent(uint8_t ref, uint8_t flags) {
    const struct aek_layer_map *entry = map_from_ref(ref);
    if (entry == NULL) {
        return -AEK_ERR_INVALID_LAYER;
    }
    if ((flags & ~AEK_INTENT_KNOWN_MASK) != 0) {
        return -AEK_ERR_INVALID_PAYLOAD;
    }
    if (!layer_state_valid()) {
        return -AEK_ERR_KEYBOARD_UNAVAILABLE;
    }

    clear_smart_app_owned_layer();
    current_app_layer = entry->zmk_id;
    smart_app_selected_ref = (ref == 0) ? AEK_NONE_REF : ref;

    if (ref == 0) {
        current_app_layer = 0;
        return 0;
    }

    is_layer_persistent = (flags & AEK_INTENT_ONE_SHOT) == 0;
    if ((flags & AEK_INTENT_ACTIVATE_NOW) != 0) {
        zmk_keymap_layer_activate(entry->zmk_id, false);
        active_app_layer = entry->zmk_id;
    }
    return 0;
}

static bool unused_payload_bytes_zero(const uint8_t data[AEK_REPORT_SIZE], uint8_t payload_len) {
    for (size_t i = AEK_HEADER_SIZE + payload_len; i < AEK_REPORT_SIZE; i++) {
        if (data[i] != 0) {
            return false;
        }
    }
    return true;
}

static void handle_v1(const struct raw_hid_received_event *ev) {
    const uint8_t *data = ev->data;
    uint8_t type = data[3];
    uint8_t flags = data[4];
    uint8_t payload_len = data[5];
    uint16_t sequence = sys_get_le16(&data[6]);

    if (data[1] != AEK_MAJOR || data[2] > AEK_MINOR) {
        send_nack(sequence, type, AEK_ERR_UNSUPPORTED_VERSION);
        return;
    }
    if (flags != 0 || payload_len > AEK_MAX_PAYLOAD ||
        !unused_payload_bytes_zero(data, payload_len)) {
        send_nack(sequence, type, AEK_ERR_INVALID_PAYLOAD);
        return;
    }

    const uint8_t *payload = &data[AEK_HEADER_SIZE];
    switch (type) {
    case AEK_MSG_HELLO:
        if (payload_len != 4) {
            send_nack(sequence, type, AEK_ERR_INVALID_PAYLOAD);
            return;
        }
        host_nonce = sys_get_le32(payload);
        session_id++;
        if (session_id == 0) {
            session_id = 1;
        }
        state_revision = 0;
        keyboard_link_state = read_keyboard_link_state();
        refresh_physical_validity(keyboard_link_state);
        v1_negotiated = true;
        send_hello_ack(sequence);
        send_state_snapshot(sequence);
        send_physical_key_state(sequence);
        link_poll_started = true;
        k_work_reschedule(&aek_link_poll_work, K_MSEC(AEK_LINK_POLL_MS));
        return;
    case AEK_MSG_GET_STATE:
        if (!v1_negotiated) {
            send_nack(sequence, type, AEK_ERR_NOT_READY);
        } else if (payload_len != 0) {
            send_nack(sequence, type, AEK_ERR_INVALID_PAYLOAD);
        } else {
            keyboard_link_state = read_keyboard_link_state();
            refresh_physical_validity(keyboard_link_state);
            send_state_snapshot(sequence);
            send_physical_key_state(sequence);
        }
        return;
    case AEK_MSG_SET_LAYER_INTENT:
        if (!v1_negotiated) {
            send_nack(sequence, type, AEK_ERR_NOT_READY);
            return;
        }
        if (payload_len != 2) {
            send_nack(sequence, type, AEK_ERR_INVALID_PAYLOAD);
            return;
        }
        keyboard_link_state = read_keyboard_link_state();
        refresh_physical_validity(keyboard_link_state);
        {
            int rc = apply_layer_intent(payload[0], payload[1]);
            if (rc < 0) {
                send_nack(sequence, type, (uint8_t)(-rc));
            } else {
                send_ack(sequence, type);
                send_state_snapshot(sequence);
            }
        }
        return;
    default:
        send_nack(sequence, type, AEK_ERR_UNSUPPORTED_MESSAGE);
        return;
    }
}

#if IS_ENABLED(CONFIG_AEKLIPSE_HID_LEGACY_COMPAT)
static bool handle_legacy(const struct raw_hid_received_event *ev) {
    if (ev->length != AEK_REPORT_SIZE || ev->data[1] > 1 || ev->data[2] > 1) {
        return false;
    }
    uint8_t ref = ref_from_zmk_id(ev->data[0]);
    if (ref == AEK_NONE_REF) {
        return false;
    }
    uint8_t flags = ev->data[1] ? AEK_INTENT_ACTIVATE_NOW : 0;
    if (ev->data[2]) {
        flags |= AEK_INTENT_ONE_SHOT;
    }
    keyboard_link_state = read_keyboard_link_state();
    refresh_physical_validity(keyboard_link_state);
    if (apply_layer_intent(ref, flags) < 0) {
        return false;
    }

    uint8_t report[AEK_REPORT_SIZE] = {0};
    report[0] = zmk_keymap_highest_layer_active();
    raise_raw_hid_sent_event((struct raw_hid_sent_event){.data = report, .length = sizeof(report)});
    return true;
}
#endif

static int on_raw_hid_received(const zmk_event_t *eh) {
    const struct raw_hid_received_event *ev = as_raw_hid_received_event(eh);
    if (ev == NULL || ev->data == NULL || ev->length != AEK_REPORT_SIZE) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (ev->data[0] == AEK_MAGIC) {
        handle_v1(ev);
        return ZMK_EV_EVENT_BUBBLE;
    }
#if IS_ENABLED(CONFIG_AEKLIPSE_HID_LEGACY_COMPAT)
    (void)handle_legacy(ev);
#endif
    return ZMK_EV_EVENT_BUBBLE;
}

static int on_layer_state_changed(const zmk_event_t *eh) {
    if (v1_negotiated) {
        keyboard_link_state = read_keyboard_link_state();
        refresh_physical_validity(keyboard_link_state);
        send_state_snapshot(0);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int on_position_state_changed(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    keyboard_link_state = read_keyboard_link_state();
    refresh_physical_validity(keyboard_link_state);

    if (ev->position >= AEK_POSITION_COUNT) {
        invalidate_physical_state();
    } else if (physical_state_valid) {
        uint8_t mask = BIT(ev->position % 8);
        if (ev->state) {
            physical_key_bitmap[ev->position / 8] |= mask;
        } else {
            physical_key_bitmap[ev->position / 8] &= (uint8_t)~mask;
        }
    }

    if (v1_negotiated) {
        send_physical_key_state(0);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static bool is_modifier(uint16_t usage_page, uint32_t keycode) {
    return usage_page == 0x07 && keycode >= 0xE0 && keycode <= 0xE7;
}

static int on_keycode_state_changed(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->usage_page == 0x07 && ev->keycode == 0x68) {
        if (current_app_layer > 0 && active_app_layer == 0) {
            zmk_keymap_layer_activate(current_app_layer, false);
            active_app_layer = current_app_layer;
        } else if (active_app_layer > 0) {
            clear_smart_app_owned_layer();
        }
        return ZMK_EV_EVENT_HANDLED;
    }

    if (active_app_layer > 0 && !is_layer_persistent && !is_modifier(ev->usage_page, ev->keycode)) {
        clear_smart_app_owned_layer();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int on_usb_conn_state_changed(const zmk_event_t *eh) {
    const struct zmk_usb_conn_state_changed *ev = as_zmk_usb_conn_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (ev->conn_state != ZMK_USB_CONN_HID) {
        v1_negotiated = false;
        host_nonce = 0;
        state_revision = 0;
    } else if (link_poll_started) {
        k_work_reschedule(&aek_link_poll_work, K_MSEC(AEK_LINK_POLL_MS));
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static void link_poll_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    uint8_t new_state = read_keyboard_link_state();
    refresh_physical_validity(new_state);

    if (v1_negotiated && new_state != keyboard_link_state) {
        keyboard_link_state = new_state;
        send_link_state_changed();
        send_state_snapshot(0);
        send_physical_key_state(0);
    } else {
        keyboard_link_state = new_state;
    }

    if (link_poll_started) {
        k_work_reschedule(&aek_link_poll_work, K_MSEC(AEK_LINK_POLL_MS));
    }
}

ZMK_LISTENER(aeklipse_raw_hid, on_raw_hid_received);
ZMK_SUBSCRIPTION(aeklipse_raw_hid, raw_hid_received_event);
ZMK_LISTENER(aeklipse_layer_report, on_layer_state_changed);
ZMK_SUBSCRIPTION(aeklipse_layer_report, zmk_layer_state_changed);
ZMK_LISTENER(aeklipse_position_report, on_position_state_changed);
ZMK_SUBSCRIPTION(aeklipse_position_report, zmk_position_state_changed);
ZMK_LISTENER(aeklipse_keycode, on_keycode_state_changed);
ZMK_SUBSCRIPTION(aeklipse_keycode, zmk_keycode_state_changed);
ZMK_LISTENER(aeklipse_usb, on_usb_conn_state_changed);
ZMK_SUBSCRIPTION(aeklipse_usb, zmk_usb_conn_state_changed);
