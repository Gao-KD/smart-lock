#include "slp/secure.h"

#include <string.h>

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int slp_app_seal_down(slp_key_handle_t key, uint64_t nonce_eui, uint8_t *buf, size_t len,
                      size_t cap, size_t *out_len)
{
    if (buf == NULL || out_len == NULL || len < SLP_APP_HDR_LEN) {
        return SLP_ERR_ARG;
    }
    if (len + SLP_APP_MIC_LEN > cap) {
        return SLP_ERR_NOSPACE;
    }
    uint8_t nonce[SLP_NONCE_LEN];
    slp_ccm_nonce(nonce_eui, rd32(buf + 1), SLP_SEC_ENC_MIC64 | SLP_DIR_DOWN, nonce);
    int rc = slp_ccm_encrypt(key, nonce, buf, SLP_APP_HDR_LEN, buf + SLP_APP_HDR_LEN,
                             len - SLP_APP_HDR_LEN, SLP_APP_MIC_LEN);
    if (rc != SLP_OK) {
        return rc;
    }
    *out_len = len + SLP_APP_MIC_LEN;
    return SLP_OK;
}

int slp_app_open_down(slp_key_handle_t key, uint64_t nonce_eui, uint8_t *buf, size_t len,
                      size_t *plain_len)
{
    if (buf == NULL || plain_len == NULL || len < SLP_APP_HDR_LEN + SLP_APP_MIC_LEN) {
        return SLP_ERR_FORMAT;
    }
    uint8_t nonce[SLP_NONCE_LEN];
    slp_ccm_nonce(nonce_eui, rd32(buf + 1), SLP_SEC_ENC_MIC64 | SLP_DIR_DOWN, nonce);
    int rc = slp_ccm_decrypt(key, nonce, buf, SLP_APP_HDR_LEN, buf + SLP_APP_HDR_LEN,
                             len - SLP_APP_HDR_LEN, SLP_APP_MIC_LEN);
    if (rc != SLP_OK) {
        return SLP_ERR_FORMAT;
    }
    *plain_len = len - SLP_APP_MIC_LEN;
    return SLP_OK;
}

int slp_app_seal_up(slp_key_handle_t key, uint64_t lock_eui, uint32_t upseq, uint8_t *buf,
                    size_t len, size_t cap, size_t *out_len)
{
    if (buf == NULL || out_len == NULL || len < SLP_APP_HDR_LEN) {
        return SLP_ERR_ARG;
    }
    if (len + SLP_UPSEQ_LEN + SLP_APP_MIC_LEN > cap) {
        return SLP_ERR_NOSPACE;
    }
    memmove(buf + SLP_UPSEQ_LEN, buf, len);
    buf[0] = (uint8_t)upseq;
    buf[1] = (uint8_t)(upseq >> 8);
    buf[2] = (uint8_t)(upseq >> 16);
    buf[3] = (uint8_t)(upseq >> 24);
    uint8_t nonce[SLP_NONCE_LEN];
    slp_ccm_nonce(lock_eui, upseq, SLP_SEC_ENC_MIC64 | SLP_DIR_UP, nonce);
    size_t aad = SLP_UPSEQ_LEN + SLP_APP_HDR_LEN;
    int rc = slp_ccm_encrypt(key, nonce, buf, aad, buf + aad, len - SLP_APP_HDR_LEN,
                             SLP_APP_MIC_LEN);
    if (rc != SLP_OK) {
        return rc;
    }
    *out_len = len + SLP_UPSEQ_LEN + SLP_APP_MIC_LEN;
    return SLP_OK;
}

int slp_app_open_up(slp_key_handle_t key, uint64_t lock_eui, uint8_t *buf, size_t len,
                    uint32_t *upseq, size_t *plain_len)
{
    size_t aad = SLP_UPSEQ_LEN + SLP_APP_HDR_LEN;
    if (buf == NULL || plain_len == NULL || upseq == NULL || len < aad + SLP_APP_MIC_LEN) {
        return SLP_ERR_FORMAT;
    }
    uint32_t seq = rd32(buf);
    uint8_t nonce[SLP_NONCE_LEN];
    slp_ccm_nonce(lock_eui, seq, SLP_SEC_ENC_MIC64 | SLP_DIR_UP, nonce);
    if (slp_ccm_decrypt(key, nonce, buf, aad, buf + aad, len - aad, SLP_APP_MIC_LEN) != SLP_OK) {
        return SLP_ERR_FORMAT;
    }
    *upseq = seq;
    *plain_len = len - SLP_UPSEQ_LEN - SLP_APP_MIC_LEN;
    return SLP_OK;
}

int slp_frame_encode_secure(slp_key_handle_t key, uint64_t sender_eui, const slp_frame_t *f,
                            uint8_t *buf, size_t cap, size_t *out_len)
{
    if (f == NULL || !f->security || buf == NULL || out_len == NULL) {
        return SLP_ERR_ARG;
    }
    /* Encode with room for the MIC as part of the payload, then encrypt the
     * payload in place and recompute the CRC. */
    uint8_t tmp[SLP_MAX_FRAME_LEN];
    if (f->payload_len + SLP_LINK_MIC_LEN > sizeof(tmp)) {
        return SLP_ERR_ARG;
    }
    if (f->payload_len) {
        memcpy(tmp, f->payload, f->payload_len);
    }
    memset(tmp + f->payload_len, 0, SLP_LINK_MIC_LEN);
    slp_frame_t g = *f;
    g.payload = tmp;
    g.payload_len = f->payload_len + SLP_LINK_MIC_LEN;
    int rc = slp_frame_encode(&g, buf, cap, out_len);
    if (rc != SLP_OK) {
        return rc;
    }
    size_t hdr = slp_frame_header_len(&g);
    uint8_t nonce[SLP_NONCE_LEN];
    slp_ccm_nonce(sender_eui, f->frame_counter, SLP_SEC_ENC_MIC32, nonce);
    rc = slp_ccm_encrypt(key, nonce, buf, hdr, buf + hdr, f->payload_len, SLP_LINK_MIC_LEN);
    if (rc != SLP_OK) {
        return rc;
    }
    uint16_t crc = slp_crc16(buf, *out_len - 2);
    buf[*out_len - 2] = (uint8_t)crc;
    buf[*out_len - 1] = (uint8_t)(crc >> 8);
    return SLP_OK;
}

int slp_frame_open_secure(slp_key_handle_t key, uint64_t sender_eui, uint8_t *buf,
                          slp_frame_t *f)
{
    if (f == NULL || buf == NULL || !f->security || f->payload_len < SLP_LINK_MIC_LEN) {
        return SLP_ERR_FORMAT;
    }
    size_t hdr = (size_t)(f->payload - buf);
    uint8_t nonce[SLP_NONCE_LEN];
    slp_ccm_nonce(sender_eui, f->frame_counter, SLP_SEC_ENC_MIC32, nonce);
    if (slp_ccm_decrypt(key, nonce, buf, hdr, buf + hdr, f->payload_len, SLP_LINK_MIC_LEN) !=
        SLP_OK) {
        return SLP_ERR_FORMAT;
    }
    f->payload_len -= SLP_LINK_MIC_LEN;
    return SLP_OK;
}
