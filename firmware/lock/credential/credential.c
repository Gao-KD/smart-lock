#include "credential.h"

#include <string.h>

static int find(const cred_store_t *s, uint8_t type, const uint8_t *id, uint8_t id_len)
{
    for (uint16_t i = 0; i < s->count; i++) {
        const cred_t *c = &s->items[i];
        if (c->type == type && c->id_len == id_len && memcmp(c->id, id, id_len) == 0) {
            return i;
        }
    }
    return -1;
}

void cred_init(cred_store_t *s)
{
    s->count = 0;
}

slp_result_t cred_add(cred_store_t *s, const cred_t *c)
{
    if (c->id_len == 0 || c->id_len > CRED_ID_MAX) {
        return SLP_RESULT_BAD_PARAM;
    }
    if (c->valid_to != 0 && c->valid_from > c->valid_to) {
        return SLP_RESULT_BAD_PARAM;
    }
    int i = find(s, c->type, c->id, c->id_len);
    if (i >= 0) {
        s->items[i] = *c;
        return SLP_RESULT_OK;
    }
    if (s->count >= CRED_MAX) {
        return SLP_RESULT_FULL;
    }
    s->items[s->count++] = *c;
    return SLP_RESULT_OK;
}

bool cred_del(cred_store_t *s, uint8_t type, const uint8_t *id, uint8_t id_len)
{
    int i = find(s, type, id, id_len);
    if (i < 0) {
        return false;
    }
    s->items[i] = s->items[--s->count];
    return true;
}

void cred_clear(cred_store_t *s)
{
    s->count = 0;
}

const cred_t *cred_match(const cred_store_t *s, uint8_t type, const uint8_t *id,
                         uint8_t id_len, uint32_t now)
{
    int i = find(s, type, id, id_len);
    if (i < 0) {
        return NULL;
    }
    const cred_t *c = &s->items[i];
    if (c->valid_from != 0 && now < c->valid_from) {
        return NULL;
    }
    if (c->valid_to != 0 && now > c->valid_to) {
        return NULL;
    }
    return c;
}
