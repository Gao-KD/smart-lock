#include "slp/app.h"

#include <string.h>

uint16_t slp_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t slp_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

int slp_app_begin(slp_app_writer_t *w, uint8_t *buf, size_t cap, const slp_app_hdr_t *hdr)
{
    if (w == NULL || buf == NULL || hdr == NULL) {
        return SLP_ERR_ARG;
    }
    if (cap < SLP_APP_HDR_LEN) {
        return SLP_ERR_NOSPACE;
    }
    w->buf = buf;
    w->cap = cap;
    buf[0] = hdr->cmd;
    put32(buf + 1, hdr->msg_id);
    put32(buf + 5, hdr->expires_at);
    w->len = SLP_APP_HDR_LEN;
    return SLP_OK;
}

int slp_app_put(slp_app_writer_t *w, uint8_t type, const void *val, uint8_t len)
{
    if (w == NULL || (len > 0 && val == NULL)) {
        return SLP_ERR_ARG;
    }
    if (w->len + 2u + len > w->cap) {
        return SLP_ERR_NOSPACE;
    }
    w->buf[w->len++] = type;
    w->buf[w->len++] = len;
    if (len > 0) {
        memcpy(w->buf + w->len, val, len);
        w->len += len;
    }
    return SLP_OK;
}

int slp_app_put_u8(slp_app_writer_t *w, uint8_t type, uint8_t v)
{
    return slp_app_put(w, type, &v, 1);
}

int slp_app_put_u16(slp_app_writer_t *w, uint8_t type, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    return slp_app_put(w, type, b, 2);
}

int slp_app_put_u32(slp_app_writer_t *w, uint8_t type, uint32_t v)
{
    uint8_t b[4];
    put32(b, v);
    return slp_app_put(w, type, b, 4);
}

int slp_app_parse(slp_app_reader_t *r, const uint8_t *buf, size_t len, slp_app_hdr_t *hdr)
{
    if (r == NULL || buf == NULL || hdr == NULL) {
        return SLP_ERR_ARG;
    }
    if (len < SLP_APP_HDR_LEN) {
        return SLP_ERR_FORMAT;
    }
    hdr->cmd = buf[0];
    hdr->msg_id = slp_le32(buf + 1);
    hdr->expires_at = slp_le32(buf + 5);
    r->buf = buf;
    r->len = len;
    r->off = SLP_APP_HDR_LEN;
    return SLP_OK;
}

int slp_app_next(slp_app_reader_t *r, uint8_t *type, const uint8_t **val, uint8_t *len)
{
    if (r->off == r->len) {
        return 0;
    }
    if (r->off + 2u > r->len) {
        return SLP_ERR_FORMAT;
    }
    uint8_t t = r->buf[r->off];
    uint8_t l = r->buf[r->off + 1u];
    if (r->off + 2u + l > r->len) {
        return SLP_ERR_FORMAT;
    }
    *type = t;
    *len = l;
    *val = r->buf + r->off + 2u;
    r->off += 2u + l;
    return 1;
}

int slp_app_find(const slp_app_reader_t *r, uint8_t type, const uint8_t **val, uint8_t *len)
{
    slp_app_reader_t it = *r;
    it.off = SLP_APP_HDR_LEN;
    uint8_t t;
    int rc;
    while ((rc = slp_app_next(&it, &t, val, len)) == 1) {
        if (t == type) {
            return 1;
        }
    }
    return rc;
}
