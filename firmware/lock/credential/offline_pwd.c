#include "offline_pwd.h"

#include <string.h>

#include "hal.h"

static const uint16_t class_hours[8] = { 0, 1, 4, 8, 24, 72, 168, 720 };

uint16_t ofp_class_hours(uint8_t kind)
{
    return kind >= 1 && kind <= 7 ? class_hours[kind] : 0;
}

void ofp_init(ofp_state_t *s)
{
    memset(s, 0, sizeof(*s));
}

int ofp_compute(uint8_t kind, uint8_t index, uint32_t start_hour, uint8_t out[OFP_LEN])
{
    uint8_t msg[9] = { 'O', 'F', 'P', kind, index, (uint8_t)start_hour, (uint8_t)(start_hour >> 8),
                       (uint8_t)(start_hour >> 16), (uint8_t)(start_hour >> 24) };
    uint8_t mac[8];
    if (kind > 9 || index > 9 || hal_app_mac(msg, sizeof(msg), mac) != 0) {
        return -1;
    }
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | mac[i];
    }
    v %= 100000000ull;
    out[0] = (uint8_t)('0' + kind);
    out[1] = (uint8_t)('0' + index);
    for (int i = OFP_LEN - 1; i >= 2; i--) {
        out[i] = (uint8_t)('0' + v % 10);
        v /= 10;
    }
    return 0;
}

static bool used(const ofp_state_t *s, uint32_t tag)
{
    for (uint8_t i = 0; i < s->used_count; i++) {
        if (s->used[i] == tag) {
            return true;
        }
    }
    return false;
}

static void mark_used(ofp_state_t *s, uint32_t tag)
{
    s->used[s->used_head] = tag;
    s->used_head = (uint8_t)((s->used_head + 1) % OFP_USED_MAX);
    if (s->used_count < OFP_USED_MAX) {
        s->used_count++;
    }
}

ofp_verdict_t ofp_verify(ofp_state_t *s, const uint8_t *digits, uint8_t len, uint32_t now)
{
    if (len != OFP_LEN) {
        return OFP_NONE;
    }
    for (uint8_t i = 0; i < len; i++) {
        if (digits[i] < '0' || digits[i] > '9') {
            return OFP_NONE;
        }
    }
    uint8_t kind = (uint8_t)(digits[0] - '0');
    uint8_t index = (uint8_t)(digits[1] - '0');
    uint32_t window;
    if (kind == OFP_KIND_ONCE) {
        window = OFP_ONCE_HOURS;
    } else if (kind == OFP_KIND_CLEAR) {
        window = OFP_CLEAR_HOURS;
    } else if (kind <= 7) {
        window = ofp_class_hours(kind);
    } else {
        return OFP_NONE;
    }
    uint32_t now_h = now / 3600u;
    uint8_t code[OFP_LEN];
    for (uint32_t back = 0; back < window && back <= now_h; back++) {
        uint32_t h = now_h - back;
        if (ofp_compute(kind, index, h, code) != 0) {
            return OFP_NONE;
        }
        if (memcmp(code, digits, OFP_LEN) != 0) {
            continue;
        }
        uint32_t tag = (h << 4) | index;
        if (kind == OFP_KIND_CLEAR) {
            if (used(s, tag | 0x80000000u)) {
                return OFP_REJECTED;
            }
            mark_used(s, tag | 0x80000000u);
            if (h > s->revoke_hour) {
                s->revoke_hour = h;
            }
            return OFP_CLEARED;
        }
        if (s->revoke_hour != 0 && h <= s->revoke_hour) {
            return OFP_REJECTED;
        }
        if (kind == OFP_KIND_ONCE) {
            if (used(s, tag)) {
                return OFP_REJECTED;
            }
            mark_used(s, tag);
        }
        return OFP_UNLOCK;
    }
    /* Right shape but no hour matched: wrong or expired. Counts as a wrong
     * attempt for the caller only if no stored password matches either. */
    return OFP_NONE;
}
