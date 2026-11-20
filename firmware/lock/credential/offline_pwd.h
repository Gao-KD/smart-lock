/*
 * Offline passwords: the platform computes a 10-digit code from the lock's
 * end-to-end key K_app; the lock checks it locally with no network at all
 * (gateway down, lock out of radio range). Typical use: a dorm keeper reads
 * a code to a repair worker over the phone.
 *
 *   code = K D MMMMMMMM
 *     K  kind digit: 0 one-time, 1..7 time-limited (duration class), 9 clear
 *     D  index 0..9 so several codes can be issued in the same hour
 *     M  8 digits = MAC64(K_app, "OFP" | K | D | start_hour) mod 10^8
 *
 * start_hour = unix time / 3600 when the platform issued the code. The lock
 * searches start hours backwards from "now", so codes need no downlink.
 *   one-time : usable once within 6 h of issue
 *   timed    : usable any number of times for the class duration
 *   clear    : (within 24 h of issue) revokes every offline code issued in
 *              or before its hour
 * The MAC is computed by hal_app_mac (AES-CCM CBC-MAC in Secure Vault).
 */
#ifndef LOCK_OFFLINE_PWD_H
#define LOCK_OFFLINE_PWD_H

#include <stdbool.h>
#include <stdint.h>

#include "slp/app.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OFP_LEN 10
#define OFP_KIND_ONCE  0
#define OFP_KIND_CLEAR 9
#define OFP_ONCE_HOURS 6u
#define OFP_CLEAR_HOURS 24u
#define OFP_USED_MAX 16

typedef struct {
    uint32_t used[OFP_USED_MAX]; /* (start_hour << 4 | index) of spent one-time / clear codes */
    uint8_t  used_count, used_head;
    uint32_t revoke_hour;        /* codes with start_hour <= this are revoked (0 = none) */
} ofp_state_t;

/* Duration in hours of kind 1..7 (0 for other kinds). */
uint16_t ofp_class_hours(uint8_t kind);

void ofp_init(ofp_state_t *s);

/* Computes the code for (kind, index, start_hour) into out[10] (ASCII).
 * Used by tests; the platform has the same function in Go. */
int ofp_compute(uint8_t kind, uint8_t index, uint32_t start_hour, uint8_t out[OFP_LEN]);

typedef enum {
    OFP_NONE = 0,  /* not an offline code (try other passwords) */
    OFP_UNLOCK,    /* valid: open the door */
    OFP_CLEARED,   /* valid clear code: all earlier codes revoked */
    OFP_REJECTED   /* well-formed but expired / used / revoked */
} ofp_verdict_t;

/* Verifies a typed 10-digit code at unix time `now`. */
ofp_verdict_t ofp_verify(ofp_state_t *s, const uint8_t *digits, uint8_t len, uint32_t now);

#ifdef __cplusplus
}
#endif

#endif
