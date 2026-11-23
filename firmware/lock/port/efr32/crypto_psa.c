/*
 * slp/crypto_port.h on EFR32: AES-128-CCM through PSA Crypto. Key handles
 * are PSA persistent key ids (board.h BOARD_KEY_ID_*), stored wrapped in
 * Secure Vault: the protocol code never sees key bytes.
 *
 * Status: written against the PSA API, not yet compiled on a board.
 */
#include <string.h>

#include "board.h"
#include "psa/crypto.h"
#include "slp/crypto_port.h"
#include "hal_efr32.h"

static uint8_t active_slot; /* set by the binding state machine at boot */

void crypto_psa_set_active_slot(uint8_t slot) { active_slot = slot; }

slp_key_handle_t hal_efr32_app_key(void) { return BOARD_KEY_ID_APP(active_slot); }

static psa_algorithm_t ccm(size_t mic_len)
{
    return PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, mic_len);
}

int slp_ccm_encrypt(slp_key_handle_t key, const uint8_t nonce[SLP_NONCE_LEN], const uint8_t *aad,
                    size_t aad_len, uint8_t *buf, size_t len, size_t mic_len)
{
    uint8_t out[SLP_MAX_FRAME_LEN + 16];
    size_t olen = 0;
    if (len + mic_len > sizeof(out)) {
        return SLP_ERR_NOSPACE;
    }
    psa_status_t st = psa_aead_encrypt((psa_key_id_t)key, ccm(mic_len), nonce, SLP_NONCE_LEN, aad,
                                       aad_len, buf, len, out, sizeof(out), &olen);
    if (st != PSA_SUCCESS || olen != len + mic_len) {
        return SLP_ERR_ARG;
    }
    memcpy(buf, out, olen);
    return SLP_OK;
}

int slp_ccm_decrypt(slp_key_handle_t key, const uint8_t nonce[SLP_NONCE_LEN], const uint8_t *aad,
                    size_t aad_len, uint8_t *buf, size_t len, size_t mic_len)
{
    uint8_t out[SLP_MAX_FRAME_LEN + 16];
    size_t olen = 0;
    if (len < mic_len || len > sizeof(out)) {
        return SLP_ERR_FORMAT;
    }
    psa_status_t st = psa_aead_decrypt((psa_key_id_t)key, ccm(mic_len), nonce, SLP_NONCE_LEN, aad,
                                       aad_len, buf, len, out, sizeof(out), &olen);
    if (st != PSA_SUCCESS) {
        return SLP_ERR_FORMAT; /* MIC failure */
    }
    memcpy(buf, out, olen);
    return SLP_OK;
}

int slp_kdf_derive(slp_key_slot_t out_slot, const uint8_t *info, size_t info_len)
{
    /* K_join / K_wrap = HKDF-SHA256(K_dev, info), kept volatile inside
     * Secure Vault (docs/provisioning.md 5.4). Implemented with the binding
     * flow; not needed for bring-up. */
    (void)out_slot;
    (void)info;
    (void)info_len;
    return SLP_ERR_ARG;
}
