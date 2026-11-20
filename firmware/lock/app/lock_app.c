#include "lock_app.h"

#include <string.h>

#include "hal.h"

void lock_app_init(lock_app_t *a)
{
    lock_core_init(&a->core, 5);
    cred_init(&a->creds);
    slp_msgid_cache_reset(&a->dedup);
    a->batt_level = SLP_BATT_OK;
    a->evq_head = 0;
    a->evq_count = 0;
    a->evt_dropped = 0;
    a->evt_seq = 0;
}

static lock_event_t *evq_push(lock_app_t *a)
{
    if (a->evq_count == LOCK_EVT_QUEUE) {
        /* Drop the oldest: the newest state matters most. */
        a->evq_head = (uint8_t)((a->evq_head + 1) % LOCK_EVT_QUEUE);
        a->evq_count--;
        a->evt_dropped++;
    }
    lock_event_t *e = &a->evq[(a->evq_head + a->evq_count) % LOCK_EVT_QUEUE];
    a->evq_count++;
    memset(e, 0, sizeof(*e));
    e->at_unix = hal_unix_time();
    return e;
}

static slp_result_t parse_cred(const slp_app_reader_t *r, cred_t *c)
{
    const uint8_t *v;
    uint8_t l;
    memset(c, 0, sizeof(*c));
    if (slp_app_find(r, SLP_TLV_CRED_TYPE, &v, &l) != 1 || l != 1) {
        return SLP_RESULT_BAD_PARAM;
    }
    c->type = v[0];
    if (slp_app_find(r, SLP_TLV_CRED_ID, &v, &l) != 1 || l == 0 || l > CRED_ID_MAX) {
        return SLP_RESULT_BAD_PARAM;
    }
    c->id_len = l;
    memcpy(c->id, v, l);
    if (slp_app_find(r, SLP_TLV_VALID_FROM, &v, &l) == 1 && l == 4) {
        c->valid_from = slp_le32(v);
    }
    if (slp_app_find(r, SLP_TLV_VALID_TO, &v, &l) == 1 && l == 4) {
        c->valid_to = slp_le32(v);
    }
    if (slp_app_find(r, SLP_TLV_USER_ID, &v, &l) == 1 && l == 4) {
        c->user_id = slp_le32(v);
    }
    return SLP_RESULT_OK;
}

static slp_result_t execute(lock_app_t *a, const slp_app_hdr_t *h, const slp_app_reader_t *r)
{
    const uint8_t *v;
    uint8_t l;
    cred_t c;
    slp_result_t res;

    switch (h->cmd) {
    case SLP_CMD_UNLOCK: {
        uint16_t dur = 0;
        if (slp_app_find(r, SLP_TLV_DURATION_S, &v, &l) == 1 && l == 2) {
            dur = slp_le16(v);
        }
        return lock_core_unlock(&a->core, LOCK_SRC_REMOTE, dur);
    }
    case SLP_CMD_LOCKDOWN:
        return lock_core_lockdown(&a->core);
    case SLP_CMD_RELEASE:
        return lock_core_release(&a->core);
    case SLP_CMD_EMERGENCY:
        return lock_core_emergency(&a->core);
    case SLP_CMD_CRED_ADD:
        res = parse_cred(r, &c);
        return res == SLP_RESULT_OK ? cred_add(&a->creds, &c) : res;
    case SLP_CMD_CRED_DEL:
        res = parse_cred(r, &c);
        if (res != SLP_RESULT_OK) {
            return res;
        }
        cred_del(&a->creds, c.type, c.id, c.id_len); /* idempotent */
        return SLP_RESULT_OK;
    case SLP_CMD_CRED_CLEAR:
        cred_clear(&a->creds);
        return SLP_RESULT_OK;
    case SLP_CMD_TIME_SYNC:
        if (slp_app_find(r, SLP_TLV_UNIX_TIME, &v, &l) != 1 || l != 4) {
            return SLP_RESULT_BAD_PARAM;
        }
        hal_set_unix_time(slp_le32(v));
        return SLP_RESULT_OK;
    case SLP_CMD_STATUS:
        return SLP_RESULT_OK;
    default:
        /* READ_LOG, CONFIG, OTA_CHUNK: TODO */
        return SLP_RESULT_UNKNOWN;
    }
}

size_t lock_app_handle_command(lock_app_t *a, const uint8_t *req, size_t req_len,
                               uint8_t *rsp, size_t rsp_cap)
{
    slp_app_reader_t r;
    slp_app_hdr_t h;
    if (slp_app_parse(&r, req, req_len, &h) != SLP_OK) {
        return 0;
    }

    slp_result_t res;
    uint32_t now = hal_unix_time();
    if (h.expires_at != 0 && now > h.expires_at) {
        res = SLP_RESULT_EXPIRED;
    } else if (slp_msgid_seen_or_add(&a->dedup, h.msg_id)) {
        /* Retransmission after a lost ACK/result: do not execute twice.
         * The platform treats DUPLICATE as success. */
        res = SLP_RESULT_DUPLICATE;
    } else {
        res = execute(a, &h, &r);
    }

    slp_app_writer_t w;
    slp_app_hdr_t rh = { .cmd = SLP_RSP_RESULT, .msg_id = h.msg_id, .expires_at = 0 };
    if (slp_app_begin(&w, rsp, rsp_cap, &rh) != SLP_OK ||
        slp_app_put_u8(&w, SLP_TLV_RESULT, (uint8_t)res) != SLP_OK) {
        return 0;
    }
    if (h.cmd == SLP_CMD_STATUS) {
        if (slp_app_put_u16(&w, SLP_TLV_BATTERY_MV, hal_battery_mv()) != SLP_OK ||
            slp_app_put_u8(&w, SLP_TLV_LOCK_STATE, (uint8_t)a->core.state) != SLP_OK ||
            slp_app_put_u16(&w, SLP_TLV_CRED_COUNT, a->creds.count) != SLP_OK ||
            slp_app_put_u8(&w, SLP_TLV_BATT_LEVEL, a->batt_level) != SLP_OK) {
            return 0;
        }
    }
    return w.len;
}

static void queue_access(lock_app_t *a, uint8_t cred_type, const uint8_t *id, uint8_t id_len,
                         uint32_t user_id, slp_result_t res)
{
    lock_event_t *e = evq_push(a);
    e->type = SLP_EVT_ACCESS;
    e->result = (uint8_t)res;
    e->cred_type = cred_type;
    e->id_len = id_len > CRED_ID_MAX ? CRED_ID_MAX : id_len;
    memcpy(e->id, id, e->id_len);
    e->user_id = user_id;
}

slp_result_t lock_app_access(lock_app_t *a, uint8_t cred_type, const uint8_t *id, uint8_t id_len)
{
    const cred_t *c = cred_match(&a->creds, cred_type, id, id_len, hal_unix_time());
    if (c == NULL) {
        hal_indicate(HAL_IND_DENIED);
        queue_access(a, cred_type, id, id_len, 0, SLP_RESULT_DENIED);
        return SLP_RESULT_DENIED;
    }
    lock_source_t src;
    switch (cred_type) {
    case SLP_CRED_CARD_UID:    src = LOCK_SRC_CARD; break;
    case SLP_CRED_PASSWORD:    src = LOCK_SRC_PASSWORD; break;
    case SLP_CRED_FINGERPRINT: src = LOCK_SRC_FINGERPRINT; break;
    case SLP_CRED_FACE:        src = LOCK_SRC_FACE; break;
    default:                   src = LOCK_SRC_BLE; break;
    }
    slp_result_t res = lock_core_unlock(&a->core, src, 0);
    queue_access(a, cred_type, id, id_len, c->user_id, res);
    if (res == SLP_RESULT_OK && lock_app_battery_check(a) != SLP_BATT_OK) {
        hal_indicate(HAL_IND_LOW_BATT); /* extra beep pattern at the door */
    }
    return res;
}

slp_batt_level_t lock_app_battery_check(lock_app_t *a)
{
    uint16_t mv = hal_battery_mv();
    uint8_t lvl = a->batt_level;
    /* Going down uses the threshold, going up needs threshold + hysteresis. */
    if (mv < LOCK_BATT_CRITICAL_MV) {
        lvl = SLP_BATT_CRITICAL;
    } else if (mv < LOCK_BATT_LOW_MV) {
        if (lvl == SLP_BATT_OK || mv >= LOCK_BATT_CRITICAL_MV + LOCK_BATT_HYST_MV) {
            lvl = SLP_BATT_LOW;
        }
    } else if (mv >= LOCK_BATT_LOW_MV + LOCK_BATT_HYST_MV || lvl == SLP_BATT_OK) {
        lvl = SLP_BATT_OK;
    }
    if (lvl != a->batt_level) {
        a->batt_level = lvl;
        lock_event_t *e = evq_push(a);
        e->type = SLP_EVT_BATTERY;
        e->battery_mv = mv;
        e->batt_level = lvl;
    }
    return (slp_batt_level_t)a->batt_level;
}

size_t lock_app_pop_event(lock_app_t *a, uint8_t *buf, size_t cap)
{
    if (a->evq_count == 0) {
        return 0;
    }
    const lock_event_t *e = &a->evq[a->evq_head];
    slp_app_writer_t w;
    slp_app_hdr_t h = { .cmd = e->type, .msg_id = ++a->evt_seq, .expires_at = 0 };
    int rc = slp_app_begin(&w, buf, cap, &h);
    if (rc == SLP_OK) {
        rc = slp_app_put_u32(&w, SLP_TLV_UNIX_TIME, e->at_unix);
    }
    if (rc == SLP_OK && e->type == SLP_EVT_ACCESS) {
        rc = slp_app_put_u8(&w, SLP_TLV_RESULT, e->result);
        if (rc == SLP_OK) rc = slp_app_put_u8(&w, SLP_TLV_CRED_TYPE, e->cred_type);
        if (rc == SLP_OK) rc = slp_app_put(&w, SLP_TLV_CRED_ID, e->id, e->id_len);
        if (rc == SLP_OK && e->user_id) rc = slp_app_put_u32(&w, SLP_TLV_USER_ID, e->user_id);
    } else if (rc == SLP_OK) {
        rc = slp_app_put_u16(&w, SLP_TLV_BATTERY_MV, e->battery_mv);
        if (rc == SLP_OK) rc = slp_app_put_u8(&w, SLP_TLV_BATT_LEVEL, e->batt_level);
    }
    a->evq_head = (uint8_t)((a->evq_head + 1) % LOCK_EVT_QUEUE);
    a->evq_count--;
    return rc == SLP_OK ? w.len : 0;
}

size_t lock_app_heartbeat(lock_app_t *a, uint8_t *buf, size_t cap)
{
    lock_app_battery_check(a);
    slp_app_writer_t w;
    slp_app_hdr_t h = { .cmd = SLP_EVT_HEARTBEAT, .msg_id = ++a->evt_seq, .expires_at = 0 };
    if (slp_app_begin(&w, buf, cap, &h) != SLP_OK ||
        slp_app_put_u8(&w, SLP_TLV_LOCK_STATE, (uint8_t)a->core.state) != SLP_OK ||
        slp_app_put_u16(&w, SLP_TLV_BATTERY_MV, hal_battery_mv()) != SLP_OK ||
        slp_app_put_u8(&w, SLP_TLV_BATT_LEVEL, a->batt_level) != SLP_OK ||
        slp_app_put_u16(&w, SLP_TLV_CRED_COUNT, a->creds.count) != SLP_OK ||
        slp_app_put_u32(&w, SLP_TLV_UNIX_TIME, hal_unix_time()) != SLP_OK) {
        return 0;
    }
    return w.len;
}
