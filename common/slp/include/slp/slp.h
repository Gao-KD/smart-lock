/*
 * SLP - Smart Lock Protocol (Sub-GHz air interface), portable C99.
 *
 * Shared by the lock firmware and the gateway radio co-processor (RCP).
 * No dynamic allocation, no platform dependencies.
 */
#ifndef SLP_SLP_H
#define SLP_SLP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes: 0 = OK, negative = error. */
#define SLP_OK            0
#define SLP_ERR_ARG      (-1)
#define SLP_ERR_NOSPACE  (-2)
#define SLP_ERR_CRC      (-3)
#define SLP_ERR_FORMAT   (-4)
#define SLP_ERR_REPLAY   (-5)
#define SLP_ERR_EXPIRED  (-6)
#define SLP_ERR_DUP      (-7)
#define SLP_ERR_EXHAUSTED (-8)

#define SLP_PROTOCOL_VERSION 0u

/* Max MAC frame length (PHR is one byte). */
#define SLP_MAX_FRAME_LEN 127u

/* Link-layer MIC length (AES-CCM*), end-to-end MIC length. */
#define SLP_LINK_MIC_LEN 4u
#define SLP_APP_MIC_LEN  8u

/* CCM nonce length. */
#define SLP_NONCE_LEN 13u

/* Reserved short addresses. */
#define SLP_SHORT_BROADCAST   0xFFFFu
#define SLP_SHORT_UNASSIGNED  0xFFFEu
/* Group addresses occupy 0xF000..0xFFFD. */
#define SLP_GROUP_BASE        0xF000u

#ifdef __cplusplus
}
#endif

#endif /* SLP_SLP_H */
