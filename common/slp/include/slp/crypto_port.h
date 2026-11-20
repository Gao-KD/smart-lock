/*
 * Crypto backend interface. Implemented per platform:
 *   - EFR32: PSA Crypto with keys held in Secure Vault (keys never leave the chip)
 *   - host:  test backend (not provided yet)
 *
 * Keys are referenced by handle, never by raw bytes, so the protocol code
 * does not need to see key material.
 */
#ifndef SLP_CRYPTO_PORT_H
#define SLP_CRYPTO_PORT_H

#include "slp/slp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t slp_key_handle_t;

/* Key slots defined by the design doc (section "安全"). */
typedef enum {
    SLP_KEY_DEV  = 0, /* device root key, derivation only */
    SLP_KEY_LINK = 1, /* lock <-> gateway link key */
    SLP_KEY_APP  = 2, /* cloud <-> lock end-to-end key */
    SLP_KEY_GRP  = 3, /* group / broadcast key */
    SLP_KEY_BLE  = 4  /* BLE offline token key */
} slp_key_slot_t;

/* AES-128-CCM* encrypt-and-authenticate in place. mic_len is 4 or 8.
 * buf must have room for len + mic_len bytes. */
int slp_ccm_encrypt(slp_key_handle_t key, const uint8_t nonce[SLP_NONCE_LEN],
                    const uint8_t *aad, size_t aad_len,
                    uint8_t *buf, size_t len, size_t mic_len);

/* Decrypt and verify in place. len includes the trailing MIC.
 * Returns SLP_OK or SLP_ERR_FORMAT on MIC failure. */
int slp_ccm_decrypt(slp_key_handle_t key, const uint8_t nonce[SLP_NONCE_LEN],
                    const uint8_t *aad, size_t aad_len,
                    uint8_t *buf, size_t len, size_t mic_len);

/* HKDF-SHA256 derivation of a new key from SLP_KEY_DEV. */
int slp_kdf_derive(slp_key_slot_t out_slot, const uint8_t *info, size_t info_len);

#ifdef __cplusplus
}
#endif

#endif /* SLP_CRYPTO_PORT_H */
