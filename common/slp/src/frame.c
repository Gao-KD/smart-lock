#include "slp/frame.h"

#include <string.h>

static size_t addr_len(uint8_t mode)
{
    switch (mode) {
    case SLP_ADDR_NONE:  return 0;
    case SLP_ADDR_SHORT: return 2;
    case SLP_ADDR_LONG:  return 8;
    default:             return (size_t)-1;
    }
}

static void put_le(uint8_t *p, uint64_t v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

static uint64_t get_le(const uint8_t *p, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        v |= (uint64_t)p[i] << (8u * i);
    }
    return v;
}

uint16_t slp_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t slp_frame_header_len(const slp_frame_t *f)
{
    size_t dl = addr_len(f->dst_mode);
    size_t sl = addr_len(f->src_mode);
    if (dl == (size_t)-1 || sl == (size_t)-1) {
        return 0;
    }
    return 2u + 1u + 2u + dl + sl + (f->security ? 5u : 0u);
}

int slp_frame_encode(const slp_frame_t *f, uint8_t *buf, size_t cap, size_t *out_len)
{
    if (f == NULL || buf == NULL || out_len == NULL || f->type > 7u) {
        return SLP_ERR_ARG;
    }
    if (f->payload_len > 0 && f->payload == NULL) {
        return SLP_ERR_ARG;
    }
    size_t hl = slp_frame_header_len(f);
    if (hl == 0) {
        return SLP_ERR_ARG;
    }
    size_t total = hl + f->payload_len + 2u;
    if (total > SLP_MAX_FRAME_LEN) {
        return SLP_ERR_ARG;
    }
    if (total > cap) {
        return SLP_ERR_NOSPACE;
    }

    uint16_t fcf = (uint16_t)(f->type & 0x7u);
    fcf |= f->security      ? (1u << 3) : 0u;
    fcf |= f->frame_pending ? (1u << 4) : 0u;
    fcf |= f->ack_request   ? (1u << 5) : 0u;
    fcf |= (uint16_t)((f->dst_mode & 0x3u) << 8);
    fcf |= (uint16_t)((f->src_mode & 0x3u) << 10);
    fcf |= (uint16_t)((f->version & 0x3u) << 12);

    uint8_t *p = buf;
    put_le(p, fcf, 2);           p += 2;
    *p++ = f->seq;
    put_le(p, f->pan_id, 2);     p += 2;
    size_t dl = addr_len(f->dst_mode);
    put_le(p, f->dst, dl);       p += dl;
    size_t sl = addr_len(f->src_mode);
    put_le(p, f->src, sl);       p += sl;
    if (f->security) {
        put_le(p, f->frame_counter, 4); p += 4;
        *p++ = f->key_id;
    }
    if (f->payload_len > 0) {
        memcpy(p, f->payload, f->payload_len);
        p += f->payload_len;
    }
    uint16_t crc = slp_crc16(buf, (size_t)(p - buf));
    put_le(p, crc, 2);           p += 2;

    *out_len = (size_t)(p - buf);
    return SLP_OK;
}

int slp_frame_decode(const uint8_t *buf, size_t len, slp_frame_t *out)
{
    if (buf == NULL || out == NULL) {
        return SLP_ERR_ARG;
    }
    if (len < 7u || len > SLP_MAX_FRAME_LEN) {
        return SLP_ERR_FORMAT;
    }
    uint16_t crc = (uint16_t)get_le(buf + len - 2u, 2);
    if (crc != slp_crc16(buf, len - 2u)) {
        return SLP_ERR_CRC;
    }

    memset(out, 0, sizeof(*out));
    uint16_t fcf = (uint16_t)get_le(buf, 2);
    out->type          = (uint8_t)(fcf & 0x7u);
    out->security      = (fcf >> 3) & 1u;
    out->frame_pending = (fcf >> 4) & 1u;
    out->ack_request   = (fcf >> 5) & 1u;
    out->dst_mode      = (uint8_t)((fcf >> 8) & 0x3u);
    out->src_mode      = (uint8_t)((fcf >> 10) & 0x3u);
    out->version       = (uint8_t)((fcf >> 12) & 0x3u);

    if (out->version != SLP_PROTOCOL_VERSION) {
        return SLP_ERR_FORMAT;
    }
    size_t hl = slp_frame_header_len(out);
    if (hl == 0 || hl + 2u > len) {
        return SLP_ERR_FORMAT;
    }

    const uint8_t *p = buf + 2;
    out->seq = *p++;
    out->pan_id = (uint16_t)get_le(p, 2); p += 2;
    size_t dl = addr_len(out->dst_mode);
    out->dst = get_le(p, dl);             p += dl;
    size_t sl = addr_len(out->src_mode);
    out->src = get_le(p, sl);             p += sl;
    if (out->security) {
        out->frame_counter = (uint32_t)get_le(p, 4); p += 4;
        out->key_id = *p++;
    }
    out->payload = p;
    out->payload_len = len - hl - 2u;
    if (out->security && out->payload_len < SLP_LINK_MIC_LEN) {
        return SLP_ERR_FORMAT;
    }
    return SLP_OK;
}
