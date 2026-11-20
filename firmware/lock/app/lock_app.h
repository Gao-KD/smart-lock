/*
 * Lock application layer: executes decrypted end-to-end platform commands
 * and local access attempts (card, password, ...).
 */
#ifndef LOCK_APP_H
#define LOCK_APP_H

#include <stddef.h>
#include <stdint.h>

#include "credential.h"
#include "lock_core.h"
#include "slp/security.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lock_core_t       core;
    cred_store_t      creds;
    slp_msgid_cache_t dedup;
} lock_app_t;

void lock_app_init(lock_app_t *a);

/* Executes one plaintext app message (after the end-to-end MIC check) and
 * writes the SLP_RSP_RESULT message into rsp. Returns the response length,
 * or 0 if no response can be built. */
size_t lock_app_handle_command(lock_app_t *a, const uint8_t *req, size_t req_len,
                               uint8_t *rsp, size_t rsp_cap);

/* Local access with a credential presented at the door. */
slp_result_t lock_app_access(lock_app_t *a, uint8_t cred_type, const uint8_t *id, uint8_t id_len);

#ifdef __cplusplus
}
#endif

#endif /* LOCK_APP_H */
