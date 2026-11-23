/*
 * End-to-end application message (cloud <-> lock), carried inside a DATA
 * frame payload. The gateway never sees the plaintext.
 *
 *   CMD(1) MSG_ID(4) EXPIRES_AT(4) TLV... [APP MIC(8) added by crypto layer]
 *
 * EXPIRES_AT is a Unix timestamp in seconds (0 = never expires).
 * TLV: TYPE(1) LEN(1) VALUE(LEN). Little-endian integers.
 */
#ifndef SLP_APP_H
#define SLP_APP_H

#include "slp/slp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SLP_APP_HDR_LEN 9u

/* Downlink commands. */
typedef enum {
    SLP_CMD_UNLOCK       = 0x01, /* remote unlock (opt TLV: DURATION_S) */
    SLP_CMD_LOCKDOWN     = 0x02, /* freeze: refuse all user credentials */
    SLP_CMD_RELEASE      = 0x03, /* leave lockdown */
    SLP_CMD_EMERGENCY    = 0x04, /* fire-alarm: keep unlocked until RELEASE */
    SLP_CMD_CRED_ADD     = 0x10,
    SLP_CMD_CRED_DEL     = 0x11,
    SLP_CMD_CRED_CLEAR   = 0x12,
    SLP_CMD_USER_DEL     = 0x13, /* delete every credential of USER_ID (move-out) */
    SLP_CMD_USER_FREEZE  = 0x14, /* FREEZE=1/0: suspend / resume a member (rent arrears) */
    SLP_CMD_READ_LOG     = 0x20, /* LOG_SEQ = first, COUNT: re-send stored records */
    SLP_CMD_CONFIG       = 0x30, /* any subset of the CFG_* TLVs */
    SLP_CMD_TIME_SYNC    = 0x31,
    SLP_CMD_OTA_CHUNK    = 0x40,
    SLP_CMD_STATUS       = 0x50,
    /* Uplink. */
    SLP_RSP_RESULT       = 0x80, /* TLV RESULT + echo of MSG_ID */
    SLP_EVT_ACCESS       = 0x90, /* unlock attempt with a credential (granted or denied) */
    SLP_EVT_ALARM        = 0x91, /* TLV ALARM (slp_alarm_t) */
    SLP_EVT_BATTERY      = 0x92, /* battery level changed (low / critical / ok) */
    SLP_EVT_HEARTBEAT    = 0x93, /* periodic status: state, battery, credentials, LOG_SEQ */
    SLP_EVT_DOOR         = 0x94, /* door sensor / inside double lock changed */
    SLP_EVT_UNLOCK_REQ   = 0x95  /* visitor pressed the call button: REQ_ID, window */
} slp_cmd_t;

/* TLV types. */
typedef enum {
    SLP_TLV_RESULT      = 0x01, /* u8, slp_result_t */
    SLP_TLV_DURATION_S  = 0x02, /* u16 */
    SLP_TLV_CRED_TYPE   = 0x03, /* u8, slp_cred_type_t */
    SLP_TLV_CRED_ID     = 0x04, /* bytes: card UID (4/7/10), password hash, ... */
    SLP_TLV_VALID_FROM  = 0x05, /* u32 unix s */
    SLP_TLV_VALID_TO    = 0x06, /* u32 unix s */
    SLP_TLV_UNIX_TIME   = 0x07, /* u32 unix s */
    SLP_TLV_BATTERY_MV  = 0x08, /* u16 */
    SLP_TLV_USER_ID     = 0x09, /* u32, platform user id */
    SLP_TLV_LOCK_STATE  = 0x0A, /* u8, lock_state_t */
    SLP_TLV_CRED_COUNT  = 0x0B, /* u16 */
    SLP_TLV_BATT_LEVEL  = 0x0C, /* u8, slp_batt_level_t */
    SLP_TLV_USES        = 0x0D, /* u16, remaining uses (0 = unlimited, 1 = one-time) */
    SLP_TLV_SCHEDULE    = 0x0E, /* 5 bytes: weekday mask (bit0 = Mon), start min, end min (u16) */
    SLP_TLV_CRED_FLAGS  = 0x0F, /* u8, SLP_CRED_F_* */
    SLP_TLV_FREEZE      = 0x10, /* u8, 1 = freeze, 0 = unfreeze */
    SLP_TLV_ALARM       = 0x11, /* u8, slp_alarm_t */
    SLP_TLV_LOG_SEQ     = 0x12, /* u32, lock's record sequence number */
    SLP_TLV_COUNT       = 0x13, /* u16 */
    SLP_TLV_DOOR        = 0x14, /* u8, 0 = closed, 1 = open */
    SLP_TLV_DOUBLE_LOCK = 0x15, /* u8, 1 = inside double lock (deadbolt) thrown */
    SLP_TLV_REQ_ID      = 0x16, /* u16, unlock request id */
    SLP_TLV_HB_REASON   = 0x17, /* u8, slp_hb_reason_t */
    SLP_TLV_SOURCE      = 0x18, /* u8, lock_source_t of an access record */
    SLP_TLV_REPLAYED    = 0x19, /* u8, 1 = record re-sent by READ_LOG */
    SLP_TLV_FLAGS       = 0x1A, /* u8, lock status flags SLP_LF_* */
    /* CONFIG */
    SLP_TLV_CFG_AUTOLOCK_S   = 0x40, /* u16, open time before auto relock */
    SLP_TLV_CFG_VOLUME       = 0x41, /* u8, 0 mute .. 3 loud */
    SLP_TLV_CFG_REMOTE       = 0x42, /* u8, 1 = remote unlock allowed without request */
    SLP_TLV_CFG_WRONG_LIMIT  = 0x43, /* u8, failed attempts before keypad/reader lockout */
    SLP_TLV_CFG_LOCKOUT_S    = 0x44, /* u16, lockout duration */
    SLP_TLV_CFG_HEARTBEAT_S  = 0x45, /* u16 */
    SLP_TLV_CFG_DOOR_ALARM_S = 0x46  /* u16, door-left-open alarm, 0 = off */
} slp_tlv_type_t;

typedef enum {
    SLP_CRED_CARD_UID    = 1, /* campus card: UID only, as decided */
    SLP_CRED_PASSWORD    = 2,
    SLP_CRED_FINGERPRINT = 3, /* template index in the fingerprint module */
    SLP_CRED_FACE        = 4, /* template index in the face module */
    SLP_CRED_BLE_TOKEN   = 5,
    SLP_CRED_OFFLINE_PWD = 6  /* access records only: platform-computed offline password */
} slp_cred_type_t;

/* Credential flags. */
#define SLP_CRED_F_DURESS 0x01u /* unlocks normally, raises a silent DURESS alarm */
#define SLP_CRED_F_FROZEN 0x02u /* member suspended */
#define SLP_CRED_F_ADMIN  0x04u /* dorm keeper: also works during LOCKDOWN */

/* Lock status flags (TLV FLAGS). */
#define SLP_LF_DOOR_OPEN   0x01u
#define SLP_LF_DOUBLE_LOCK 0x02u
#define SLP_LF_LOCKED_OUT  0x04u
#define SLP_LF_UNSYNCED    0x08u

typedef enum {
    SLP_ALARM_WRONG_ATTEMPTS = 1, /* repeated wrong card / password: reader locked out */
    SLP_ALARM_TAMPER         = 2, /* panel pried off */
    SLP_ALARM_DURESS         = 3, /* duress credential used */
    SLP_ALARM_DOOR_OPEN      = 4, /* door left open */
    SLP_ALARM_MECH_KEY       = 5, /* opened with the mechanical override key */
    SLP_ALARM_HW_FAULT       = 6, /* clutch motor stalled */
    SLP_ALARM_RF_JAM         = 7  /* repeated fake wake-up preambles: sniff rate reduced */
} slp_alarm_t;

typedef enum {
    SLP_HB_PERIODIC  = 0,
    SLP_HB_RECONNECT = 1, /* first heartbeat after beacons came back */
    SLP_HB_BOOT      = 2
} slp_hb_reason_t;

typedef enum {
    SLP_RESULT_OK         = 0,
    SLP_RESULT_EXPIRED    = 1,
    SLP_RESULT_DUPLICATE  = 2,
    SLP_RESULT_DENIED     = 3,
    SLP_RESULT_BAD_PARAM  = 4,
    SLP_RESULT_FULL       = 5,
    SLP_RESULT_HW_FAULT   = 6,
    SLP_RESULT_UNKNOWN    = 7,
    SLP_RESULT_DOUBLE_LOCKED = 8, /* inside deadbolt thrown: outside unlock impossible */
    SLP_RESULT_LOCKED_OUT = 9,    /* too many wrong attempts, reader temporarily disabled */
    SLP_RESULT_FROZEN     = 10,   /* member suspended */
    SLP_RESULT_SCHEDULE   = 11,   /* outside the credential's weekly time window */
    SLP_RESULT_NEED_REQUEST = 12  /* remote unlock disabled unless answering a request */
} slp_result_t;

typedef enum {
    SLP_BATT_OK       = 0,
    SLP_BATT_LOW      = 1, /* replace soon (weeks left) */
    SLP_BATT_CRITICAL = 2  /* replace now: remote features may be shed */
} slp_batt_level_t;

typedef struct {
    uint8_t  cmd;
    uint32_t msg_id;
    uint32_t expires_at;
} slp_app_hdr_t;

/* ---- writer ---- */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
} slp_app_writer_t;

int slp_app_begin(slp_app_writer_t *w, uint8_t *buf, size_t cap, const slp_app_hdr_t *hdr);
int slp_app_put(slp_app_writer_t *w, uint8_t type, const void *val, uint8_t len);
int slp_app_put_u8(slp_app_writer_t *w, uint8_t type, uint8_t v);
int slp_app_put_u16(slp_app_writer_t *w, uint8_t type, uint16_t v);
int slp_app_put_u32(slp_app_writer_t *w, uint8_t type, uint32_t v);

/* ---- reader ---- */
typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t off;
} slp_app_reader_t;

int slp_app_parse(slp_app_reader_t *r, const uint8_t *buf, size_t len, slp_app_hdr_t *hdr);
/* Returns 1 and fills type/val/len for the next TLV, 0 at end, <0 on error. */
int slp_app_next(slp_app_reader_t *r, uint8_t *type, const uint8_t **val, uint8_t *len);
/* Finds the first TLV of the given type (scans from the start). */
int slp_app_find(const slp_app_reader_t *r, uint8_t type, const uint8_t **val, uint8_t *len);

uint16_t slp_le16(const uint8_t *p);
uint32_t slp_le32(const uint8_t *p);

#ifdef __cplusplus
}
#endif

#endif /* SLP_APP_H */
