#include "lock_app.h"

#include <string.h>

#include "hal.h"

void lock_app_init(lock_app_t *a)
{
    memset(a, 0, sizeof(*a));
    a->cfg = (lock_cfg_t){ .autolock_s = 5, .volume = 2, .remote_allowed = 1, .wrong_limit = 5,
                           .lockout_s = 180, .heartbeat_s = 1800, .door_alarm_s = 120,
                           .tz_offset_min = 480 };
    lock_core_init(&a->core, a->cfg.autolock_s);
    cred_init(&a->creds);
    ofp_init(&a->ofp);
    slp_msgid_cache_reset(&a->dedup);
    a->batt_level = SLP_BATT_OK;
}

/* ---- records and the uplink queue ---- */

static lock_event_t *evq_push_copy(lock_app_t *a, const lock_event_t *src)
{
    if (a->evq_count == LOCK_EVT_QUEUE) {
        /* Drop the oldest: it stays in the log ring and the platform pulls
         * it back with READ_LOG when it sees the gap in LOG_SEQ. */
        a->evq_head = (uint8_t)((a->evq_head + 1) % LOCK_EVT_QUEUE);
        a->evq_count--;
        a->evt_dropped++;
    }
    lock_event_t *e = &a->evq[(a->evq_head + a->evq_count) % LOCK_EVT_QUEUE];
    a->evq_count++;
    *e = *src;
    return e;
}

/* Records an event in the log ring and queues it for the radio. */
static void record(lock_app_t *a, lock_event_t *e)
{
    e->at_unix = hal_unix_time();
    e->log_seq = ++a->log_seq;
    a->log[e->log_seq % LOCK_LOG_SIZE] = *e;
    evq_push_copy(a, e);
}

static void raise_alarm(lock_app_t *a, uint8_t alarm, uint8_t cred_type, uint32_t user_id)
{
    lock_event_t e = { .type = SLP_EVT_ALARM, .alarm = alarm, .cred_type = cred_type, .user_id = user_id };
    record(a, &e);
}

static void record_access(lock_app_t *a, uint8_t cred_type, lock_source_t src, const uint8_t *id,
                          uint8_t id_len, uint32_t user_id, slp_result_t res)
{
    lock_event_t e = { .type = SLP_EVT_ACCESS, .result = (uint8_t)res, .cred_type = cred_type,
                       .source = (uint8_t)src, .user_id = user_id };
    e.id_len = id_len > CRED_ID_MAX ? CRED_ID_MAX : id_len;
    if (e.id_len) {
        memcpy(e.id, id, e.id_len);
    }
    record(a, &e);
}

uint16_t lock_app_local_min_of_week(const lock_app_t *a, uint32_t unix_s)
{
    int64_t local = (int64_t)unix_s + (int64_t)a->cfg.tz_offset_min * 60;
    if (local < 0) {
        local = 0;
    }
    uint32_t days = (uint32_t)(local / 86400);
    uint32_t min = (uint32_t)(local % 86400) / 60;
    uint32_t dow = (days + 3) % 7; /* 1970-01-01 was a Thursday; 0 = Monday */
    return (uint16_t)(dow * 1440 + min);
}

uint8_t lock_app_flags(const lock_app_t *a)
{
    uint8_t f = 0;
    if (a->door_open) f |= SLP_LF_DOOR_OPEN;
    if (a->core.double_locked) f |= SLP_LF_DOUBLE_LOCK;
    if (a->locked_out_until_ms) f |= SLP_LF_LOCKED_OUT;
    return f;
}

/* ---- commands ---- */

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
    if (slp_app_find(r, SLP_TLV_USES, &v, &l) == 1 && l == 2) {
        c->uses_left = slp_le16(v);
    }
    if (slp_app_find(r, SLP_TLV_CRED_FLAGS, &v, &l) == 1 && l == 1) {
        c->flags = v[0];
    }
    if (slp_app_find(r, SLP_TLV_SCHEDULE, &v, &l) == 1) {
        if (l != 5) {
            return SLP_RESULT_BAD_PARAM;
        }
        c->week_mask = v[0] & 0x7F;
        c->day_start_min = slp_le16(v + 1);
        c->day_end_min = slp_le16(v + 3);
    }
    return SLP_RESULT_OK;
}

static bool find_u32(const slp_app_reader_t *r, uint8_t type, uint32_t *out)
{
    const uint8_t *v;
    uint8_t l;
    if (slp_app_find(r, type, &v, &l) == 1 && l == 4) {
        *out = slp_le32(v);
        return true;
    }
    return false;
}

static bool find_u16(const slp_app_reader_t *r, uint8_t type, uint16_t *out)
{
    const uint8_t *v;
    uint8_t l;
    if (slp_app_find(r, type, &v, &l) == 1 && l == 2) {
        *out = slp_le16(v);
        return true;
    }
    return false;
}

static bool find_u8(const slp_app_reader_t *r, uint8_t type, uint8_t *out)
{
    const uint8_t *v;
    uint8_t l;
    if (slp_app_find(r, type, &v, &l) == 1 && l == 1) {
        *out = v[0];
        return true;
    }
    return false;
}

static slp_result_t apply_config(lock_app_t *a, const slp_app_reader_t *r)
{
    lock_cfg_t c = a->cfg;
    uint16_t v16;
    uint8_t v8;
    if (find_u16(r, SLP_TLV_CFG_AUTOLOCK_S, &v16)) {
        if (v16 < 1 || v16 > 600) return SLP_RESULT_BAD_PARAM;
        c.autolock_s = v16;
    }
    if (find_u8(r, SLP_TLV_CFG_VOLUME, &v8)) {
        if (v8 > 3) return SLP_RESULT_BAD_PARAM;
        c.volume = v8;
    }
    if (find_u8(r, SLP_TLV_CFG_REMOTE, &v8)) {
        c.remote_allowed = v8 ? 1 : 0;
    }
    if (find_u8(r, SLP_TLV_CFG_WRONG_LIMIT, &v8)) {
        c.wrong_limit = v8;
    }
    if (find_u16(r, SLP_TLV_CFG_LOCKOUT_S, &v16)) {
        if (v16 > 3600) return SLP_RESULT_BAD_PARAM;
        c.lockout_s = v16;
    }
    if (find_u16(r, SLP_TLV_CFG_HEARTBEAT_S, &v16)) {
        if (v16 < 10) return SLP_RESULT_BAD_PARAM;
        c.heartbeat_s = v16;
    }
    if (find_u16(r, SLP_TLV_CFG_DOOR_ALARM_S, &v16)) {
        c.door_alarm_s = v16;
    }
    a->cfg = c;
    a->core.default_open_s = c.autolock_s;
    return SLP_RESULT_OK;
}

static uint16_t read_log(lock_app_t *a, uint32_t from, uint16_t count)
{
    uint16_t n = 0;
    if (from == 0) {
        from = 1;
    }
    for (uint32_t s = from; s <= a->log_seq && n < count && n < LOCK_EVT_QUEUE; s++) {
        const lock_event_t *e = &a->log[s % LOCK_LOG_SIZE];
        if (e->log_seq != s) {
            continue; /* overwritten by newer records */
        }
        lock_event_t cp = *e;
        cp.replayed = 1;
        evq_push_copy(a, &cp);
        n++;
    }
    return n;
}

static slp_result_t remote_unlock(lock_app_t *a, const slp_app_reader_t *r)
{
    uint16_t dur = 0, req = 0;
    find_u16(r, SLP_TLV_DURATION_S, &dur);
    bool answers = find_u16(r, SLP_TLV_REQ_ID, &req) && a->req_until_ms != 0 && req == a->req_id;
    if (!a->cfg.remote_allowed && !answers) {
        return SLP_RESULT_NEED_REQUEST;
    }
    slp_result_t res = lock_core_unlock(&a->core, LOCK_SRC_REMOTE, dur);
    if (answers && res == SLP_RESULT_OK) {
        a->req_until_ms = 0;
    }
    if (res == SLP_RESULT_HW_FAULT) {
        raise_alarm(a, SLP_ALARM_HW_FAULT, 0, 0);
    }
    return res;
}

static slp_result_t execute(lock_app_t *a, const slp_app_hdr_t *h, const slp_app_reader_t *r,
                            uint16_t *count_out)
{
    cred_t c;
    slp_result_t res;
    uint32_t user;
    uint8_t v8;

    switch (h->cmd) {
    case SLP_CMD_UNLOCK:
        return remote_unlock(a, r);
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
    case SLP_CMD_USER_DEL:
        if (!find_u32(r, SLP_TLV_USER_ID, &user) || user == 0) {
            return SLP_RESULT_BAD_PARAM;
        }
        *count_out = cred_del_user(&a->creds, user);
        return SLP_RESULT_OK;
    case SLP_CMD_USER_FREEZE:
        if (!find_u32(r, SLP_TLV_USER_ID, &user) || user == 0 || !find_u8(r, SLP_TLV_FREEZE, &v8)) {
            return SLP_RESULT_BAD_PARAM;
        }
        *count_out = cred_freeze_user(&a->creds, user, v8 != 0);
        return SLP_RESULT_OK;
    case SLP_CMD_READ_LOG: {
        uint32_t from = 0;
        uint16_t count = LOCK_EVT_QUEUE;
        find_u32(r, SLP_TLV_LOG_SEQ, &from);
        find_u16(r, SLP_TLV_COUNT, &count);
        *count_out = read_log(a, from, count);
        return SLP_RESULT_OK;
    }
    case SLP_CMD_CONFIG:
        return apply_config(a, r);
    case SLP_CMD_TIME_SYNC:
        if (!find_u32(r, SLP_TLV_UNIX_TIME, &user)) {
            return SLP_RESULT_BAD_PARAM;
        }
        hal_set_unix_time(user);
        return SLP_RESULT_OK;
    case SLP_CMD_STATUS:
        return SLP_RESULT_OK;
    default:
        /* OTA_CHUNK: TODO */
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
    uint16_t count = 0xFFFF;
    uint32_t now = hal_unix_time();
    if (h.expires_at != 0 && now > h.expires_at) {
        res = SLP_RESULT_EXPIRED;
    } else if (slp_msgid_seen_or_add(&a->dedup, h.msg_id)) {
        /* Retransmission after a lost ACK/result: do not execute twice.
         * The platform treats DUPLICATE as success. */
        res = SLP_RESULT_DUPLICATE;
    } else {
        res = execute(a, &h, &r, &count);
    }

    slp_app_writer_t w;
    slp_app_hdr_t rh = { .cmd = SLP_RSP_RESULT, .msg_id = h.msg_id, .expires_at = 0 };
    if (slp_app_begin(&w, rsp, rsp_cap, &rh) != SLP_OK ||
        slp_app_put_u8(&w, SLP_TLV_RESULT, (uint8_t)res) != SLP_OK) {
        return 0;
    }
    int rc = SLP_OK;
    if (count != 0xFFFF) {
        rc = slp_app_put_u16(&w, SLP_TLV_COUNT, count);
    }
    if (rc == SLP_OK && (h.cmd == SLP_CMD_STATUS || h.cmd == SLP_CMD_CONFIG)) {
        rc = slp_app_put_u16(&w, SLP_TLV_BATTERY_MV, hal_battery_mv());
        if (rc == SLP_OK) rc = slp_app_put_u8(&w, SLP_TLV_LOCK_STATE, (uint8_t)a->core.state);
        if (rc == SLP_OK) rc = slp_app_put_u16(&w, SLP_TLV_CRED_COUNT, a->creds.count);
        if (rc == SLP_OK) rc = slp_app_put_u8(&w, SLP_TLV_BATT_LEVEL, a->batt_level);
        if (rc == SLP_OK) rc = slp_app_put_u8(&w, SLP_TLV_FLAGS, lock_app_flags(a));
        if (rc == SLP_OK) rc = slp_app_put_u32(&w, SLP_TLV_LOG_SEQ, a->log_seq);
        if (rc == SLP_OK) rc = slp_app_put_u16(&w, SLP_TLV_CFG_AUTOLOCK_S, a->cfg.autolock_s);
        if (rc == SLP_OK) rc = slp_app_put_u8(&w, SLP_TLV_CFG_REMOTE, a->cfg.remote_allowed);
    }
    if (rc == SLP_OK && (h.cmd == SLP_CMD_USER_DEL || h.cmd == SLP_CMD_CRED_ADD ||
                         h.cmd == SLP_CMD_CRED_DEL || h.cmd == SLP_CMD_CRED_CLEAR)) {
        rc = slp_app_put_u16(&w, SLP_TLV_CRED_COUNT, a->creds.count);
    }
    return rc == SLP_OK ? w.len : 0;
}

/* ---- local access ---- */

static lock_source_t source_of(uint8_t cred_type)
{
    switch (cred_type) {
    case SLP_CRED_CARD_UID:    return LOCK_SRC_CARD;
    case SLP_CRED_PASSWORD:
    case SLP_CRED_OFFLINE_PWD: return LOCK_SRC_PASSWORD;
    case SLP_CRED_FINGERPRINT: return LOCK_SRC_FINGERPRINT;
    case SLP_CRED_FACE:        return LOCK_SRC_FACE;
    default:                   return LOCK_SRC_BLE;
    }
}

/* Reader / keypad lockout after repeated failures. */
static bool locked_out(lock_app_t *a)
{
    if (a->locked_out_until_ms == 0) {
        return false;
    }
    if ((int32_t)(hal_millis() - a->locked_out_until_ms) >= 0) {
        a->locked_out_until_ms = 0;
        a->wrong_count = 0;
        return false;
    }
    return true;
}

static void count_failure(lock_app_t *a, uint8_t cred_type)
{
    if (a->cfg.wrong_limit == 0) {
        return;
    }
    if (++a->wrong_count >= a->cfg.wrong_limit) {
        a->locked_out_until_ms = hal_millis() + (uint32_t)a->cfg.lockout_s * 1000u;
        if (a->locked_out_until_ms == 0) {
            a->locked_out_until_ms = 1;
        }
        hal_indicate(HAL_IND_ALARM);
        raise_alarm(a, SLP_ALARM_WRONG_ATTEMPTS, cred_type, 0);
    }
}

/* Opens for a matched, checked credential and records it. */
static slp_result_t grant(lock_app_t *a, uint8_t cred_type, lock_source_t src, const uint8_t *rec_id,
                          uint8_t rec_len, uint32_t user, uint8_t flags)
{
    if (flags & SLP_CRED_F_ADMIN) {
        src = LOCK_SRC_ADMIN; /* dorm keeper: allowed during lockdown */
    }
    slp_result_t res = lock_core_unlock(&a->core, src, 0);
    record_access(a, cred_type, src, rec_id, rec_len, user, res);
    if (res == SLP_RESULT_HW_FAULT) {
        raise_alarm(a, SLP_ALARM_HW_FAULT, cred_type, user);
        return res;
    }
    if (res != SLP_RESULT_OK) {
        return res;
    }
    a->wrong_count = 0;
    if (flags & SLP_CRED_F_DURESS) {
        /* Opens as usual so the person at the door is not endangered; the
         * platform gets a silent alarm. */
        raise_alarm(a, SLP_ALARM_DURESS, cred_type, user);
    }
    if (lock_app_battery_check(a) != SLP_BATT_OK) {
        hal_indicate(HAL_IND_LOW_BATT); /* extra beep pattern at the door */
    }
    return res;
}

static slp_result_t refuse(lock_app_t *a, uint8_t cred_type, const uint8_t *rec_id, uint8_t rec_len,
                           uint32_t user, slp_result_t why)
{
    hal_indicate(HAL_IND_DENIED);
    record_access(a, cred_type, source_of(cred_type), rec_id, rec_len, user, why);
    if (why == SLP_RESULT_DENIED) {
        count_failure(a, cred_type);
    }
    return why;
}

static slp_result_t use_credential(lock_app_t *a, cred_t *c, const uint8_t *rec_id, uint8_t rec_len)
{
    uint32_t now = hal_unix_time();
    slp_result_t chk = cred_check(c, now, lock_app_local_min_of_week(a, now));
    uint8_t type = c->type;
    uint32_t user = c->user_id;
    uint8_t flags = c->flags;
    if (chk != SLP_RESULT_OK) {
        return refuse(a, type, rec_id, rec_len, user, chk);
    }
    if (a->core.double_locked) {
        /* Not a wrong attempt: the person inside has bolted the door. */
        return refuse(a, type, rec_id, rec_len, user, SLP_RESULT_DOUBLE_LOCKED);
    }
    slp_result_t res = grant(a, type, source_of(type), rec_id, rec_len, user, flags);
    if (res == SLP_RESULT_OK) {
        cred_consume(&a->creds, c); /* one-time / N-time credentials */
    }
    return res;
}

slp_result_t lock_app_access(lock_app_t *a, uint8_t cred_type, const uint8_t *id, uint8_t id_len)
{
    if (locked_out(a)) {
        hal_indicate(HAL_IND_DENIED);
        return SLP_RESULT_LOCKED_OUT;
    }
    cred_t *c = cred_find(&a->creds, cred_type, id, id_len);
    if (c == NULL) {
        return refuse(a, cred_type, id, id_len, 0, SLP_RESULT_DENIED);
    }
    return use_credential(a, c, id, id_len);
}

slp_result_t lock_app_keypad(lock_app_t *a, const uint8_t *digits, uint8_t len)
{
    if (locked_out(a)) {
        hal_indicate(HAL_IND_DENIED);
        return SLP_RESULT_LOCKED_OUT;
    }
    /* Records never carry password digits. */
    cred_t *c = cred_find_password(&a->creds, digits, len);
    if (c != NULL) {
        return use_credential(a, c, NULL, 0);
    }
    uint8_t tag[2] = { digits[0], len > 1 ? digits[1] : 0 }; /* kind + index of an offline code */
    switch (ofp_verify(&a->ofp, digits, len, hal_unix_time())) {
    case OFP_UNLOCK:
        if (a->core.double_locked) {
            return refuse(a, SLP_CRED_OFFLINE_PWD, tag, 2, 0, SLP_RESULT_DOUBLE_LOCKED);
        }
        return grant(a, SLP_CRED_OFFLINE_PWD, LOCK_SRC_PASSWORD, tag, 2, 0, 0);
    case OFP_CLEARED:
        hal_indicate(HAL_IND_OK);
        record_access(a, SLP_CRED_OFFLINE_PWD, LOCK_SRC_PASSWORD, tag, 2, 0, SLP_RESULT_OK);
        return SLP_RESULT_OK;
    case OFP_REJECTED:
        return refuse(a, SLP_CRED_OFFLINE_PWD, tag, 2, 0, SLP_RESULT_EXPIRED);
    case OFP_NONE:
    default:
        return refuse(a, SLP_CRED_PASSWORD, NULL, 0, 0, SLP_RESULT_DENIED);
    }
}

slp_result_t lock_app_ble(lock_app_t *a, const uint8_t *token, uint8_t len)
{
    if (len != LOCK_BLE_TOKEN_LEN) {
        return refuse(a, SLP_CRED_BLE_TOKEN, NULL, 0, 0, SLP_RESULT_BAD_PARAM);
    }
    uint8_t msg[12] = { 'B', 'L', 'E' };
    memcpy(msg + 3, token, 9);
    uint8_t mac[8];
    uint32_t user = slp_le32(token);
    if (hal_app_mac(msg, sizeof(msg), mac) != 0 || memcmp(mac, token + 9, 8) != 0) {
        return refuse(a, SLP_CRED_BLE_TOKEN, NULL, 0, 0, SLP_RESULT_DENIED);
    }
    uint32_t valid_to = slp_le32(token + 4);
    if (hal_unix_time() > valid_to) {
        return refuse(a, SLP_CRED_BLE_TOKEN, NULL, 0, user, SLP_RESULT_EXPIRED);
    }
    if (a->core.double_locked) {
        return refuse(a, SLP_CRED_BLE_TOKEN, NULL, 0, user, SLP_RESULT_DOUBLE_LOCKED);
    }
    return grant(a, SLP_CRED_BLE_TOKEN, LOCK_SRC_BLE, NULL, 0, user, token[8] & SLP_CRED_F_ADMIN);
}

/* ---- sensors ---- */

static void record_door(lock_app_t *a)
{
    lock_event_t e = { .type = SLP_EVT_DOOR, .door = a->door_open, .double_lock = a->core.double_locked };
    record(a, &e);
}

void lock_app_door(lock_app_t *a, bool open)
{
    if (open == a->door_open) {
        return;
    }
    a->door_open = open;
    a->door_open_ms = hal_millis();
    a->door_alarmed = false;
    record_door(a);
    if (!open && a->core.state == LOCK_ST_UNLOCKED) {
        lock_core_relock_now(&a->core); /* door closed again: lock at once */
    }
}

void lock_app_double_lock(lock_app_t *a, bool on)
{
    if (on == a->core.double_locked) {
        return;
    }
    a->core.double_locked = on;
    record_door(a);
}

void lock_app_rf_jam(lock_app_t *a)
{
    raise_alarm(a, SLP_ALARM_RF_JAM, 0, 0);
}

void lock_app_tamper(lock_app_t *a)
{
    hal_indicate(HAL_IND_ALARM);
    raise_alarm(a, SLP_ALARM_TAMPER, 0, 0);
}

void lock_app_mech_key(lock_app_t *a)
{
    raise_alarm(a, SLP_ALARM_MECH_KEY, 0, 0);
}

uint16_t lock_app_doorbell(lock_app_t *a)
{
    a->req_id = (uint16_t)(a->req_id + 1);
    if (a->req_id == 0) {
        a->req_id = 1;
    }
    a->req_until_ms = hal_millis() + LOCK_REQ_WINDOW_S * 1000u;
    if (a->req_until_ms == 0) {
        a->req_until_ms = 1;
    }
    lock_event_t e = { .type = SLP_EVT_UNLOCK_REQ, .req_id = a->req_id };
    record(a, &e);
    hal_indicate(HAL_IND_OK);
    return a->req_id;
}

void lock_app_tick(lock_app_t *a)
{
    lock_core_tick(&a->core);
    uint32_t now = hal_millis();
    (void)locked_out(a); /* clears an expired lockout */
    if (a->req_until_ms && (int32_t)(now - a->req_until_ms) >= 0) {
        a->req_until_ms = 0;
    }
    if (a->door_open && !a->door_alarmed && a->cfg.door_alarm_s &&
        now - a->door_open_ms >= (uint32_t)a->cfg.door_alarm_s * 1000u) {
        a->door_alarmed = true;
        hal_indicate(HAL_IND_ALARM);
        raise_alarm(a, SLP_ALARM_DOOR_OPEN, 0, 0);
    }
}

int32_t lock_app_time_sync(lock_app_t *a, uint32_t unix_s)
{
    int32_t d = (int32_t)(unix_s - hal_unix_time());
    if (d > 2 || d < -2) {
        hal_set_unix_time(unix_s);
        a->clock_fixes++;
        return d;
    }
    return 0;
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
        lock_event_t e = { .type = SLP_EVT_BATTERY, .battery_mv = mv, .batt_level = lvl };
        record(a, &e);
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
    if (rc == SLP_OK) rc = slp_app_put_u32(&w, SLP_TLV_UNIX_TIME, e->at_unix);
    if (rc == SLP_OK) rc = slp_app_put_u32(&w, SLP_TLV_LOG_SEQ, e->log_seq);
    if (rc == SLP_OK && e->replayed) rc = slp_app_put_u8(&w, SLP_TLV_REPLAYED, 1);
    switch (e->type) {
    case SLP_EVT_ACCESS:
        if (rc == SLP_OK) rc = slp_app_put_u8(&w, SLP_TLV_RESULT, e->result);
        if (rc == SLP_OK) rc = slp_app_put_u8(&w, SLP_TLV_CRED_TYPE, e->cred_type);
        if (rc == SLP_OK) rc = slp_app_put_u8(&w, SLP_TLV_SOURCE, e->source);
        if (rc == SLP_OK && e->id_len) rc = slp_app_put(&w, SLP_TLV_CRED_ID, e->id, e->id_len);
        if (rc == SLP_OK && e->user_id) rc = slp_app_put_u32(&w, SLP_TLV_USER_ID, e->user_id);
        break;
    case SLP_EVT_ALARM:
        if (rc == SLP_OK) rc = slp_app_put_u8(&w, SLP_TLV_ALARM, e->alarm);
        if (rc == SLP_OK && e->cred_type) rc = slp_app_put_u8(&w, SLP_TLV_CRED_TYPE, e->cred_type);
        if (rc == SLP_OK && e->user_id) rc = slp_app_put_u32(&w, SLP_TLV_USER_ID, e->user_id);
        break;
    case SLP_EVT_DOOR:
        if (rc == SLP_OK) rc = slp_app_put_u8(&w, SLP_TLV_DOOR, e->door);
        if (rc == SLP_OK) rc = slp_app_put_u8(&w, SLP_TLV_DOUBLE_LOCK, e->double_lock);
        break;
    case SLP_EVT_UNLOCK_REQ:
        if (rc == SLP_OK) rc = slp_app_put_u16(&w, SLP_TLV_REQ_ID, e->req_id);
        if (rc == SLP_OK) rc = slp_app_put_u16(&w, SLP_TLV_DURATION_S, (uint16_t)LOCK_REQ_WINDOW_S);
        break;
    default: /* SLP_EVT_BATTERY */
        if (rc == SLP_OK) rc = slp_app_put_u16(&w, SLP_TLV_BATTERY_MV, e->battery_mv);
        if (rc == SLP_OK) rc = slp_app_put_u8(&w, SLP_TLV_BATT_LEVEL, e->batt_level);
        break;
    }
    a->evq_head = (uint8_t)((a->evq_head + 1) % LOCK_EVT_QUEUE);
    a->evq_count--;
    return rc == SLP_OK ? w.len : 0;
}

size_t lock_app_heartbeat(lock_app_t *a, uint8_t *buf, size_t cap, slp_hb_reason_t reason)
{
    lock_app_battery_check(a);
    slp_app_writer_t w;
    slp_app_hdr_t h = { .cmd = SLP_EVT_HEARTBEAT, .msg_id = ++a->evt_seq, .expires_at = 0 };
    int rc = slp_app_begin(&w, buf, cap, &h);
    if (rc == SLP_OK) rc = slp_app_put_u8(&w, SLP_TLV_LOCK_STATE, (uint8_t)a->core.state);
    if (rc == SLP_OK) rc = slp_app_put_u16(&w, SLP_TLV_BATTERY_MV, hal_battery_mv());
    if (rc == SLP_OK) rc = slp_app_put_u8(&w, SLP_TLV_BATT_LEVEL, a->batt_level);
    if (rc == SLP_OK) rc = slp_app_put_u16(&w, SLP_TLV_CRED_COUNT, a->creds.count);
    if (rc == SLP_OK) rc = slp_app_put_u32(&w, SLP_TLV_UNIX_TIME, hal_unix_time());
    if (rc == SLP_OK) rc = slp_app_put_u8(&w, SLP_TLV_FLAGS, lock_app_flags(a));
    if (rc == SLP_OK) rc = slp_app_put_u32(&w, SLP_TLV_LOG_SEQ, a->log_seq);
    if (rc == SLP_OK) rc = slp_app_put_u8(&w, SLP_TLV_HB_REASON, (uint8_t)reason);
    return rc == SLP_OK ? w.len : 0;
}

uint32_t lock_app_heartbeat_ms(const lock_app_t *a, uint32_t rnd)
{
    uint32_t ms = (uint32_t)a->cfg.heartbeat_s * 1000u;
    if (a->batt_level == SLP_BATT_CRITICAL) {
        ms *= 4u; /* save the last of the battery for opening the door */
    }
    uint32_t jitter = ms / 10u;
    return ms - jitter + (jitter ? rnd % (2u * jitter + 1u) : 0u);
}
