/*
 * Credential store: campus card UIDs, passwords, fingerprint/face template
 * indexes, BLE tokens. Campus cards are identified by UID only (no sector
 * keys / PSAM), per the design decision.
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

typedef struct {
    uint8_t  type;                /* slp_cred_type_t */
    uint8_t  id_len;
    uint8_t  id[CRED_ID_MAX];
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
/* Returns the matching credential if it is valid at `now`, else NULL. */
const cred_t *cred_match(const cred_store_t *s, uint8_t type, const uint8_t *id,
                         uint8_t id_len, uint32_t now);

#ifdef __cplusplus
}
#endif

#endif /* LOCK_CREDENTIAL_H */
