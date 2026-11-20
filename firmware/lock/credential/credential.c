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

static bool all_digits(const uint8_t *p, uint8_t n)
{
    for (uint8_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') {
            return false;
        }
    }
    return true;
}

slp_result_t cred_add(cred_store_t *s, const cred_t *c)
{
    if (c->id_len == 0 || c->id_len > CRED_ID_MAX) {
        return SLP_RESULT_BAD_PARAM;
    }
    if (c->valid_to != 0 && c->valid_from > c->valid_to) {
        return SLP_RESULT_BAD_PARAM;
    }
    if (c->type == SLP_CRED_PASSWORD &&
        (c->id_len < CRED_PWD_MIN || c->id_len > CRED_PWD_MAX || !all_digits(c->id, c->id_len))) {
        return SLP_RESULT_BAD_PARAM;
    }
    if (c->day_end_min > 24 * 60 || c->day_start_min > 24 * 60) {
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

static void remove_at(cred_store_t *s, uint16_t i)
{
    s->items[i] = s->items[--s->count];
}

bool cred_del(cred_store_t *s, uint8_t type, const uint8_t *id, uint8_t id_len)
{
    int i = find(s, type, id, id_len);
    if (i < 0) {
        return false;
    }
    remove_at(s, (uint16_t)i);
    return true;
}

void cred_clear(cred_store_t *s)
{
    s->count = 0;
}

uint16_t cred_del_user(cred_store_t *s, uint32_t user_id)
{
    uint16_t n = 0;
    for (uint16_t i = 0; i < s->count;) {
        if (s->items[i].user_id == user_id) {
            remove_at(s, i);
            n++;
        } else {
            i++;
        }
    }
    return n;
}

uint16_t cred_freeze_user(cred_store_t *s, uint32_t user_id, bool freeze)
{
    uint16_t n = 0;
    for (uint16_t i = 0; i < s->count; i++) {
        cred_t *c = &s->items[i];
        if (c->user_id != user_id) {
            continue;
        }
        if (freeze) {
            c->flags |= SLP_CRED_F_FROZEN;
        } else {
            c->flags &= (uint8_t)~SLP_CRED_F_FROZEN;
        }
        n++;
    }
    return n;
}

cred_t *cred_find(cred_store_t *s, uint8_t type, const uint8_t *id, uint8_t id_len)
{
    int i = find(s, type, id, id_len);
    return i < 0 ? NULL : &s->items[i];
}

cred_t *cred_find_password(cred_store_t *s, const uint8_t *digits, uint8_t len)
{
    cred_t *best = NULL;
    if (len > CRED_KEYPAD_MAX) {
        return NULL;
    }
    for (uint16_t i = 0; i < s->count; i++) {
        cred_t *c = &s->items[i];
        if (c->type != SLP_CRED_PASSWORD || c->id_len > len) {
            continue;
        }
        for (uint8_t off = 0; off + c->id_len <= len; off++) {
            if (memcmp(digits + off, c->id, c->id_len) == 0) {
                if (best == NULL || c->id_len > best->id_len) {
                    best = c;
                }
                break;
            }
        }
    }
    return best;
}

slp_result_t cred_check(const cred_t *c, uint32_t now, uint16_t local_min_of_week)
{
    if (c->flags & SLP_CRED_F_FROZEN) {
        return SLP_RESULT_FROZEN;
    }
    if ((c->valid_from != 0 && now < c->valid_from) || (c->valid_to != 0 && now > c->valid_to)) {
        return SLP_RESULT_EXPIRED;
    }
    uint8_t day = (uint8_t)(local_min_of_week / (24 * 60)); /* 0 = Monday */
    uint16_t min = (uint16_t)(local_min_of_week % (24 * 60));
    if (c->week_mask != 0 && !(c->week_mask & (1u << day))) {
        return SLP_RESULT_SCHEDULE;
    }
    if (c->day_end_min != 0) {
        bool in = c->day_start_min <= c->day_end_min
                      ? (min >= c->day_start_min && min < c->day_end_min)
                      : (min >= c->day_start_min || min < c->day_end_min); /* over midnight */
        if (!in) {
            return SLP_RESULT_SCHEDULE;
        }
    }
    return SLP_RESULT_OK;
}

void cred_consume(cred_store_t *s, cred_t *c)
{
    if (c->uses_left == 0) {
        return;
    }
    if (--c->uses_left == 0) {
        remove_at(s, (uint16_t)(c - s->items));
    }
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
