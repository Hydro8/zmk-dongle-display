#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

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
#define AEK_MSG_ACK 0x90
#define AEK_MSG_NACK 0x91

#define AEK_INTENT_ACTIVATE_NOW BIT(0)
#define AEK_INTENT_ONE_SHOT BIT(1)
#define AEK_INTENT_KNOWN_MASK (AEK_INTENT_ACTIVATE_NOW | AEK_INTENT_ONE_SHOT)

#define AEK_ERR_MALFORMED 1
#define AEK_ERR_UNSUPPORTED_VERSION 2
#define AEK_ERR_UNKNOWN_TYPE 3
#define AEK_ERR_INVALID_LAYER 4
#define AEK_ERR_NOT_NEGOTIATED 5
#define AEK_ERR_INVALID_FLAGS 6

#define AEK_CAP_STATE_SNAPSHOT BIT(0)
#define AEK_CAP_LAYER_INTENT BIT(1)
#define AEK_CAP_LEGACY_COMPAT BIT(2)

struct aek_layer_map {
    const char *layer_id;
    uint8_t wire_ref;
    zmk_keymap_layer_id_t zmk_id;
};

/* Explicit catalog: host persistence uses layer_id; the wire uses wire_ref;
 * firmware resolves wire_ref to the current ZMK layer id. Numeric equality is incidental. */
static const struct aek_layer_map layer_map[] = {
    {"base", 0, 0}, {"num-lock", 1, 1}, {"symbols", 2, 2}, {"vim-nav", 3, 3},
    {"nav-num", 4, 4}, {"num-row", 5, 5}, {"fn-media", 6, 6},
    {"app-autocad", 7, 7}, {"app-word", 8, 8}, {"app-excel", 9, 9}, {"app-calc", 10, 10},
};

/* Shared with behavior_app_layer.c. These are internal ZMK ids, never wire identities. */
uint8_t current_app_layer = 0;
uint8_t active_app_layer = 0;
bool is_layer_persistent = false;

static bool v1_negotiated;
static uint16_t session_id;
static uint16_t state_revision;
static uint32_t host_nonce;
static uint8_t smart_app_selected_ref = AEK_NONE_REF;

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
    uint8_t bitmap[16];
    zmk_keymap_layer_index_t highest_index = zmk_keymap_highest_layer_active();
    zmk_keymap_layer_id_t highest_id = zmk_keymap_layer_index_to_id(highest_index);
    uint8_t highest_ref = ref_from_zmk_id(highest_id);

    build_active_bitmap(bitmap);
    state_revision++;
    sys_put_le16(state_revision, &payload[0]);
    payload[2] = 1;
    payload[3] = 1; /* local/dongle state available; aggregate BLE detail remains a later gate */
    payload[4] = highest_ref;
    payload[5] = smart_app_selected_ref;
    payload[6] = 16;
    memcpy(&payload[7], bitmap, sizeof(bitmap));
    payload[23] = 0;
    send_frame(AEK_MSG_STATE_SNAPSHOT, sequence, payload, sizeof(payload));
}

static void send_hello_ack(uint16_t sequence) {
    uint8_t payload[14] = {0};
    uint16_t capabilities = AEK_CAP_STATE_SNAPSHOT | AEK_CAP_LAYER_INTENT;
#if IS_ENABLED(CONFIG_AEKLIPSE_HID_LEGACY_COMPAT)
    capabilities |= AEK_CAP_LEGACY_COMPAT;
#endif
    sys_put_le32(host_nonce, &payload[0]);
    sys_put_le16(session_id, &payload[4]);
    sys_put_le16(capabilities, &payload[6]);
    sys_put_le16(1, &payload[8]);
    sys_put_le32(0x53454C31u, &payload[10]); /* SEL1 catalog fingerprint */
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
        return -AEK_ERR_INVALID_FLAGS;
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
    if (flags != 0 || payload_len > AEK_MAX_PAYLOAD) {
        send_nack(sequence, type, AEK_ERR_MALFORMED);
        return;
    }

    const uint8_t *payload = &data[AEK_HEADER_SIZE];
    switch (type) {
    case AEK_MSG_HELLO:
        if (payload_len != 4) {
            send_nack(sequence, type, AEK_ERR_MALFORMED);
            return;
        }
        host_nonce = sys_get_le32(payload);
        session_id++;
        if (session_id == 0) {
            session_id = 1;
        }
        state_revision = 0;
        v1_negotiated = true;
        send_hello_ack(sequence);
        send_state_snapshot(sequence);
        return;
    case AEK_MSG_GET_STATE:
        if (!v1_negotiated) {
            send_nack(sequence, type, AEK_ERR_NOT_NEGOTIATED);
        } else if (payload_len != 0) {
            send_nack(sequence, type, AEK_ERR_MALFORMED);
        } else {
            send_state_snapshot(sequence);
        }
        return;
    case AEK_MSG_SET_LAYER_INTENT:
        if (!v1_negotiated) {
            send_nack(sequence, type, AEK_ERR_NOT_NEGOTIATED);
            return;
        }
        if (payload_len != 2) {
            send_nack(sequence, type, AEK_ERR_MALFORMED);
            return;
        }
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
        send_nack(sequence, type, AEK_ERR_UNKNOWN_TYPE);
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
    (void)apply_layer_intent(ref, flags);

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
        send_state_snapshot(0);
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

ZMK_LISTENER(aeklipse_raw_hid, on_raw_hid_received);
ZMK_SUBSCRIPTION(aeklipse_raw_hid, raw_hid_received_event);
ZMK_LISTENER(aeklipse_layer_report, on_layer_state_changed);
ZMK_SUBSCRIPTION(aeklipse_layer_report, zmk_layer_state_changed);
ZMK_LISTENER(aeklipse_keycode, on_keycode_state_changed);
ZMK_SUBSCRIPTION(aeklipse_keycode, zmk_keycode_state_changed);
