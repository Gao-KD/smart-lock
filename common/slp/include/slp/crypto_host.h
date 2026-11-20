/*
 * Host (PC) crypto backend for slp/crypto_port.h: software AES-128 and
 * AES-128-CCM* (RFC 3610, L = 2), used by unit tests and the simulator.
 *
 * On EFR32 the same interface is implemented with PSA Crypto and keys held
 * in Secure Vault; this file is never linked into the lock firmware image.
 */
#ifndef SLP_CRYPTO_HOST_H
#define SLP_CRYPTO_HOST_H

#include "slp/crypto_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SLP_HOST_MAX_KEYS 8192u

/* Installs raw key material for a handle (host backend only). */
int slp_host_key_set(slp_key_handle_t h, const uint8_t key[16]);

/* Single-block AES-128 encryption (FIPS-197). */
void slp_aes128_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

/* CCM* with raw keys, used by the handle-based API and by tests. */
int slp_ccm_encrypt_raw(const uint8_t key[16], const uint8_t nonce[SLP_NONCE_LEN],
                        const uint8_t *aad, size_t aad_len,
                        uint8_t *buf, size_t len, size_t mic_len);
int slp_ccm_decrypt_raw(const uint8_t key[16], const uint8_t nonce[SLP_NONCE_LEN],
                        const uint8_t *aad, size_t aad_len,
                        uint8_t *buf, size_t len, size_t mic_len);

#ifdef __cplusplus
}
#endif

#endif /* SLP_CRYPTO_HOST_H */
