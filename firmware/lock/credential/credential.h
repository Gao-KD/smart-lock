/*
 * Credential store: campus card UIDs, keypad passwords, fingerprint/face
 * template indexes (reserved), BLE tokens. Campus cards are identified by
 * UID only (no sector keys / PSAM), per the design decision.
 *
 * Every credential belongs to a member (platform user id) and can carry:
 *   - an absolute validity window (valid_from .. valid_to)
 *   - a weekly schedule (weekday mask + minutes of the day), e.g. a cleaner
 *     on weekdays 08:00-10:00
 *   - a use counter (one-time / N-time passwords for visitors, repairs)
 *   - flags: duress (silent alarm), frozen (member suspended), admin
 *
 * Kept in RAM and mirrored to NVM3 (TODO: persistence in port/efr32).
 */
#ifndef LOCK_CREDENTIAL_H
#define LOCK_CREDENTIAL_H

#include <stdbool.h>
#include <stdint.h>

#include "slp/app.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CRED_MAX
#define CRED_MAX 1000
#endif
#define CRED_ID_MAX 16
/* Keypad passwords: 6..10 digits stored, up to 16 digits typed (decoy
 * digits around the real password, "虚位密码"). */
#define CRED_PWD_MIN 6
#define CRED_PWD_MAX 10
#define CRED_KEYPAD_MAX 16

typedef struct {
    uint8_t  type;                /* slp_cred_type_t */
    uint8_t  id_len;
    uint8_t  id[CRED_ID_MAX];     /* card UID, or ASCII digits of a password */
    uint8_t  flags;               /* SLP_CRED_F_* */
    uint8_t  week_mask;           /* bit0 = Monday .. bit6 = Sunday, 0 = every day */
    uint16_t day_start_min;       /* local minutes, schedule start (inclusive) */
    uint16_t day_end_min;         /* schedule end (exclusive), 0 = no daily window */
    uint16_t uses_left;           /* 0 = unlimited */
    uint32_t valid_from;          /* unix s, 0 = no lower bound */
    uint32_t valid_to;            /* unix s, 0 = no upper bound */
    uint32_t user_id;
} cred_t;

typedef struct {
    cred_t   items[CRED_MAX];
    uint16_t count;
} cred_store_t;

void cred_init(cred_store_t *s);
/* Adds or replaces (same type + id). */
slp_result_t cred_add(cred_store_t *s, const cred_t *c);
bool cred_del(cred_store_t *s, uint8_t type, const uint8_t *id, uint8_t id_len);
void cred_clear(cred_store_t *s);
/* Member management. Return the number of credentials touched. */
uint16_t cred_del_user(cred_store_t *s, uint32_t user_id);
uint16_t cred_freeze_user(cred_store_t *s, uint32_t user_id, bool freeze);

/* Finds a credential by exact type + id. */
cred_t *cred_find(cred_store_t *s, uint8_t type, const uint8_t *id, uint8_t id_len);
/* Finds the password contained in the typed digits (decoy digits allowed
 * before and after it). The longest match wins. */
cred_t *cred_find_password(cred_store_t *s, const uint8_t *digits, uint8_t len);

/* Checks validity window, weekly schedule and the frozen flag at `now`
 * (unix s); local_min_of_week = minutes since Monday 00:00 local time. */
slp_result_t cred_check(const cred_t *c, uint32_t now, uint16_t local_min_of_week);
/* Consumes one use of a counted credential; deletes it at zero. */
void cred_consume(cred_store_t *s, cred_t *c);

/* Back-compatible helper: matching credential valid at `now`, else NULL. */
const cred_t *cred_match(const cred_store_t *s, uint8_t type, const uint8_t *id,
                         uint8_t id_len, uint32_t now);

#ifdef __cplusplus
}
#endif

#endif /* LOCK_CREDENTIAL_H */
