#include "lock_app.h"

#include <string.h>

#include "hal.h"

void lock_app_init(lock_app_t *a)
{
    lock_core_init(&a->core, 5);
    cred_init(&a->creds);
    slp_msgid_cache_reset(&a->dedup);
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
        if (slp_app_put_u16(&w, SLP_TLV_BATTERY_MV, hal_battery_mv()) != SLP_OK) {
            return 0;
        }
    }
    return w.len;
}

slp_result_t lock_app_access(lock_app_t *a, uint8_t cred_type, const uint8_t *id, uint8_t id_len)
{
    const cred_t *c = cred_match(&a->creds, cred_type, id, id_len, hal_unix_time());
    if (c == NULL) {
        hal_indicate(HAL_IND_DENIED);
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
    /* TODO: append to event log and queue SLP_EVT_ACCESS uplink. */
    return lock_core_unlock(&a->core, src, 0);
}
