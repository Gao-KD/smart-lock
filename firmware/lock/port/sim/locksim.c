/*
 * locksim - virtual locks + simulated 433 MHz air + simulated radio
 * co-processors, for running the whole system on a PC.
 *
 * Every virtual lock runs the real lock firmware logic (lock_app, lock_core,
 * credential store, lock_mac, the SLP frame codec, AES-CCM* two-layer
 * security, anti-replay) compiled for the host. Only the hardware is
 * simulated (port/sim/hal_sim.c) and the radio is replaced by this file:
 *
 *   gateway host --UDP--> [RCP + air + locks in this process]
 *
 * One UDP port per gateway (port, port+1, ...) carries the RCP wire format
 * (sim_net.h). A control port takes text commands (swipe a card, pull the
 * battery down, block the radio, replay a captured frame, ...) and returns a
 * snapshot of every lock for the web UI.
 *
 * Timing: each PERIOD message is the gateway's plan for one listen period
 * (default 1000 ms). Frames are delivered at slot start (slot * slot_ms
 * after the period start), only to locks whose (channel, slot) matches: a
 * lock is asleep outside its own slot, exactly like on the real radio.
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "hal_sim.h"
#include "lock_app.h"
#include "lock_mac.h"
#include "sim_net.h"
#include "slp/crypto_host.h"
#include "slp/secure.h"

#define MAX_GW      32
#define MAX_LOCKS   3900
#define MAX_PENDING 8192
#define H_LINK(i)     ((slp_key_handle_t)(2 * (i)))
#define H_APP(i)      ((slp_key_handle_t)(2 * (i) + 1))
#define H_GRP_LINK(g) ((slp_key_handle_t)(8000 + 2 * (g)))
#define H_GRP_APP(g)  ((slp_key_handle_t)(8001 + 2 * (g)))

typedef struct {
    uint64_t       eui;
    int            floor, room, gw;
    hal_sim_lock_t hw;
    lock_app_t     app;
    lock_mac_t     mac;
    bool           joined;
    bool           rf_on;
    slp_txctr_t    txc;
    uint32_t       upseq;
    uint32_t       join_due_ms;
    uint32_t       hb_due_ms;
    int            drop_acks;
    uint8_t        captured[SLP_MAX_FRAME_LEN]; /* last unicast frame heard by an eavesdropper */
    size_t         captured_len;
    lock_state_t   last_state;
} vlock_t;

typedef struct {
    int                fd;
    int                port;
    struct sockaddr_in peer;
    bool               have_peer;
    uint64_t           eui;
    uint32_t           session;
    uint16_t           pan;
    int                period_ms, slot_ms;
    int                first, count;
    uint32_t           periods;
    uint32_t           period_start_ms;
} gwctx_t;

typedef struct {
    uint32_t due_ms;
    int      gw;
    uint8_t  ch, slot;
    uint32_t ref;
    uint8_t  len;
    uint8_t  frame[SLP_MAX_FRAME_LEN];
} pending_tx_t;

static vlock_t *locks;
static int nlocks;
static gwctx_t gws[MAX_GW];
static int ngw;
static pending_tx_t pend[MAX_PENDING];
static int npend;

static int building = 1, floors = 12, rooms = 30, floors_per_gw = 6;
static int base_port = 9100, ctrl_port = 9200;
static int hb_s = 30;
static int loss_pct = 0;
static const char *bind_host = "127.0.0.1";

static int ctrl_fd;
static struct sockaddr_in log_peer;
static bool have_log_peer;

static uint32_t now_ms(void) { return hal_sim_now_ms(); }

static bool chance(int pct) { return pct > 0 && (rand() % 100) < pct; }

/* ---- logging to stdout and to the subscribed UI ---- */
static void vlog(const vlock_t *lk, const char *fmt, ...)
{
    char text[400], line[480];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (lk) {
        snprintf(line, sizeof(line), "LOG %016llx %s", (unsigned long long)lk->eui, text);
        printf("[%d%02d] %s\n", lk->floor, lk->room, text);
    } else {
        snprintf(line, sizeof(line), "LOG - %s", text);
        printf("[sim] %s\n", text);
    }
    fflush(stdout);
    if (have_log_peer) {
        sendto(ctrl_fd, line, strlen(line), 0, (struct sockaddr *)&log_peer, sizeof(log_peer));
    }
}

static const char *cmd_name(uint8_t c)
{
    switch (c) {
    case SLP_CMD_UNLOCK: return "远程开门";
    case SLP_CMD_LOCKDOWN: return "封锁";
    case SLP_CMD_RELEASE: return "解除封锁/恢复";
    case SLP_CMD_EMERGENCY: return "应急常开";
    case SLP_CMD_CRED_ADD: return "添加卡";
    case SLP_CMD_CRED_DEL: return "删除卡";
    case SLP_CMD_CRED_CLEAR: return "清空凭证";
    case SLP_CMD_TIME_SYNC: return "校时";
    case SLP_CMD_STATUS: return "状态查询";
    default: return "未知指令";
    }
}

static const char *result_name(uint8_t r)
{
    static const char *n[] = { "OK", "EXPIRED(已过期)", "DUPLICATE(重复, 不再执行)", "DENIED",
                               "BAD_PARAM", "FULL", "HW_FAULT", "UNKNOWN" };
    return r < 8 ? n[r] : "?";
}

/* ---- uplink ---- */
static void send_rx(gwctx_t *g, uint8_t ch, int8_t rssi, const uint8_t *frame, size_t len)
{
    if (!g->have_peer) {
        return;
    }
    uint8_t m[4 + SLP_MAX_FRAME_LEN];
    m[0] = SIM_MSG_RX;
    m[1] = ch;
    m[2] = (uint8_t)rssi;
    m[3] = (uint8_t)len;
    memcpy(m + 4, frame, len);
    sendto(g->fd, m, 4 + len, 0, (struct sockaddr *)&g->peer, sizeof(g->peer));
}

static int8_t lock_rssi(const vlock_t *lk)
{
    int f = (lk->floor - 1) % floors_per_gw;
    return (int8_t)(-52 - f * 5 - lk->room / 3 - rand() % 4);
}

/* Seals a plaintext app message end to end, wraps it in a secured link frame
 * and transmits it to the gateway. */
static void lock_uplink(vlock_t *lk, uint8_t *msg, size_t len)
{
    int i = (int)(lk - locks);
    gwctx_t *g = &gws[lk->gw];
    uint8_t buf[SLP_MAX_FRAME_LEN];
    size_t sealed;
    memcpy(buf, msg, len);
    if (slp_app_seal_up(H_APP(i), lk->eui, ++lk->upseq, buf, len, sizeof(buf), &sealed) != SLP_OK) {
        vlog(lk, "上行封装失败");
        return;
    }
    uint32_t fc, pv;
    bool np;
    if (slp_txctr_take(&lk->txc, &fc, &np, &pv) != SLP_OK) {
        return;
    }
    slp_frame_t f = {
        .type = SLP_FT_DATA, .security = true, .seq = (uint8_t)fc, .pan_id = lk->mac.pan_id,
        .dst_mode = SLP_ADDR_SHORT, .dst = 0x0000, .src_mode = SLP_ADDR_SHORT,
        .src = lk->mac.short_addr, .frame_counter = fc, .key_id = SLP_KEY_LINK,
        .payload = buf, .payload_len = sealed,
    };
    uint8_t frame[SLP_MAX_FRAME_LEN];
    size_t flen;
    if (slp_frame_encode_secure(H_LINK(i), lk->eui, &f, frame, sizeof(frame), &flen) != SLP_OK) {
        vlog(lk, "上行帧过长");
        return;
    }
    send_rx(g, lk->mac.channel, lock_rssi(lk), frame, flen);
}

static void flush_events(vlock_t *lk)
{
    uint8_t m[96];
    size_t n;
    while (lk->joined && lk->rf_on && (n = lock_app_pop_event(&lk->app, m, sizeof(m))) > 0) {
        lock_uplink(lk, m, n);
    }
}

static void send_join_req(vlock_t *lk)
{
    gwctx_t *g = &gws[lk->gw];
    uint8_t pl[1] = { SIM_MAC_JOIN_REQ };
    slp_frame_t f = {
        .type = SLP_FT_MAC_CMD, .pan_id = g->pan, .dst_mode = SLP_ADDR_SHORT, .dst = 0x0000,
        .src_mode = SLP_ADDR_LONG, .src = lk->eui, .payload = pl, .payload_len = 1,
    };
    uint8_t frame[64];
    size_t flen;
    if (slp_frame_encode(&f, frame, sizeof(frame), &flen) == SLP_OK) {
        send_rx(g, 0xFF, lock_rssi(lk), frame, flen); /* 0xFF = control channel */
    }
}

/* ---- downlink processing in one lock ---- */
static void on_join_accept(vlock_t *lk, const slp_frame_t *f)
{
    if (f->payload_len < 10 || f->payload[0] != SIM_MAC_JOIN_ACCEPT) {
        return;
    }
    const uint8_t *p = f->payload + 1;
    uint16_t short_addr = slp_le16(p);
    uint8_t ch = p[2];
    uint16_t slot = slp_le16(p + 3);
    uint16_t period = slp_le16(p + 5);
    uint16_t slot_ms = slp_le16(p + 7);
    lock_mac_init(&lk->mac, f->pan_id, short_addr, ch, slot, period, slot_ms);
    lk->mac.groups[0] = SLP_SHORT_BROADCAST;
    lk->mac.group_count = 1;
    lk->joined = true;
    lk->hb_due_ms = now_ms() + (uint32_t)(rand() % (hb_s * 1000));
    vlog(lk, "入网成功: 短地址 0x%04x, 信道 %u, 时隙 %u (每 %u ms 侦听一次)", short_addr, ch,
         slot, period);
}

/* Returns true if the lock sends a link-layer ACK. */
static bool lock_rx(vlock_t *lk, const uint8_t *in, size_t len, bool attacker)
{
    int i = (int)(lk - locks);
    gwctx_t *g = &gws[lk->gw];
    uint8_t buf[SLP_MAX_FRAME_LEN];
    memcpy(buf, in, len);
    slp_frame_t f;
    int rc = slp_frame_decode(buf, len, &f);
    if (rc != SLP_OK) {
        if (attacker) vlog(lk, "收到损坏的帧 (CRC 错误)，丢弃");
        return false;
    }
    if (f.type == SLP_FT_MAC_CMD && f.dst_mode == SLP_ADDR_LONG && f.dst == lk->eui) {
        on_join_accept(lk, &f);
        return false;
    }
    if (f.type != SLP_FT_DATA || !lk->joined || !lock_mac_addressed_to_me(&lk->mac, &f)) {
        return false;
    }
    bool uni = (uint16_t)f.dst == lk->mac.short_addr;
    if (uni && !attacker) {
        memcpy(lk->captured, in, len);
        lk->captured_len = len;
    }
    if (slp_frame_open_secure(uni ? H_LINK(i) : H_GRP_LINK(lk->gw), g->eui, buf, &f) != SLP_OK) {
        vlog(lk, "链路层 MIC 校验失败: 帧被篡改或伪造，丢弃 (fc=%u)", f.frame_counter);
        return false;
    }
    slp_replay_t *rp = uni ? &lk->mac.gw_replay : &lk->mac.grp_replay;
    uint32_t last = rp->last;
    if (lock_mac_replay_check(&lk->mac, &f) != SLP_OK) {
        vlog(lk, "防重放: 帧计数器 %u <= 已接收的 %u，判定为重放攻击，丢弃", f.frame_counter, last);
        return false;
    }
    bool ack = uni && f.ack_request;

    uint8_t msg[SLP_MAX_FRAME_LEN];
    size_t mlen = f.payload_len, plain;
    memcpy(msg, f.payload, mlen);
    if (slp_app_open_down(uni ? H_APP(i) : H_GRP_APP(lk->gw), uni ? lk->eui : g->eui, msg, mlen,
                          &plain) != SLP_OK) {
        vlog(lk, "端到端 MIC 校验失败 (不是平台签发的指令)，拒绝执行");
        return ack;
    }
    hal_sim_cur = &lk->hw;
    uint8_t rsp[96];
    size_t rn = lock_app_handle_command(&lk->app, msg, plain, rsp, sizeof(rsp));
    slp_app_reader_t r;
    slp_app_hdr_t h, rh;
    const uint8_t *v;
    uint8_t l;
    slp_app_parse(&r, msg, plain, &h);
    uint8_t res = 0xFF;
    if (rn && slp_app_parse(&r, rsp, rn, &rh) == SLP_OK && slp_app_find(&r, SLP_TLV_RESULT, &v, &l) == 1) {
        res = v[0];
    }
    vlog(lk, "%s指令 %s (msg %u, fc %u) -> %s, 门锁状态 %s", uni ? "" : "广播", cmd_name(h.cmd),
         h.msg_id, f.frame_counter, result_name(res), lock_state_name(lk->app.core.state));
    if (rn) {
        lock_uplink(lk, rsp, rn);
    }
    flush_events(lk);
    return ack;
}

/* ---- gateway messages ---- */
static void deliver_now(int gi, const uint8_t *frame, size_t len)
{
    gwctx_t *g = &gws[gi];
    for (int k = g->first; k < g->first + g->count; k++) {
        if (locks[k].rf_on) {
            lock_rx(&locks[k], frame, len, false);
        }
    }
}

static void send_txres(gwctx_t *g, uint32_t ref, bool acked, int8_t rssi)
{
    uint8_t m[7] = { SIM_MSG_TX_RESULT, (uint8_t)ref, (uint8_t)(ref >> 8), (uint8_t)(ref >> 16),
                     (uint8_t)(ref >> 24), acked ? 1 : 0, (uint8_t)rssi };
    if (g->have_peer) {
        sendto(g->fd, m, sizeof(m), 0, (struct sockaddr *)&g->peer, sizeof(g->peer));
    }
}

static void deliver_slot(pending_tx_t *t)
{
    gwctx_t *g = &gws[t->gw];
    bool acked = false;
    int8_t rssi = 0;
    for (int k = g->first; k < g->first + g->count; k++) {
        vlock_t *lk = &locks[k];
        if (!lk->rf_on || !lk->joined || lk->mac.channel != t->ch || lk->mac.my_slot != t->slot) {
            continue; /* asleep or on another channel: does not hear it */
        }
        if (chance(loss_pct)) {
            continue;
        }
        if (lock_rx(lk, t->frame, t->len, false)) {
            if (lk->drop_acks > 0) {
                lk->drop_acks--;
                vlog(lk, "(模拟) ACK 在空中丢失，网关会重发");
            } else if (!chance(loss_pct)) {
                acked = true;
                rssi = lock_rssi(lk);
            }
        }
    }
    send_txres(g, t->ref, acked, rssi);
}

static void on_hello(int gi, const uint8_t *m, size_t n)
{
    gwctx_t *g = &gws[gi];
    if (n < 21) {
        return;
    }
    uint64_t eui = 0;
    for (int i = 0; i < 8; i++) eui |= (uint64_t)m[1 + i] << (8 * i);
    uint32_t session = (uint32_t)m[9] | (uint32_t)m[10] << 8 | (uint32_t)m[11] << 16 | (uint32_t)m[12] << 24;
    g->period_ms = slp_le16(m + 15);
    g->slot_ms = slp_le16(m + 17);
    g->pan = slp_le16(m + 19);
    if (eui != g->eui) {
        vlog(NULL, "网关 EUI %016llx 与楼层配置不符 (期望 %016llx)，忽略", (unsigned long long)eui,
             (unsigned long long)g->eui);
        return;
    }
    if (session != g->session) {
        g->session = session;
        vlog(NULL, "网关 %d 上线 (会话 %08x)，%d 把锁开始入网", gi + 1, session, g->count);
        uint32_t t = now_ms();
        for (int k = g->first; k < g->first + g->count; k++) {
            locks[k].joined = false;
            locks[k].join_due_ms = t + 100 + (uint32_t)(rand() % 1500);
        }
    }
}

static void queue_entries(int gi, const uint8_t *m, size_t n, size_t p, uint32_t start);

static void on_period(int gi, const uint8_t *m, size_t n)
{
    gwctx_t *g = &gws[gi];
    uint32_t start = now_ms();
    if (n < 6) return;
    size_t p = 5;
    uint8_t blen = m[p++];
    if (p + blen + 2 > n) return;
    slp_frame_t bf;
    uint8_t bbuf[SLP_MAX_FRAME_LEN];
    memcpy(bbuf, m + p, blen);
    if (blen && slp_frame_decode(bbuf, blen, &bf) == SLP_OK && bf.type == SLP_FT_BEACON) {
        for (int k = g->first; k < g->first + g->count; k++) {
            if (locks[k].rf_on && locks[k].joined) {
                lock_mac_on_beacon(&locks[k].mac, start, 0);
            }
        }
    }
    p += blen;
    g->periods++;
    g->period_start_ms = start;
    queue_entries(gi, m, n, p, start);
}

/* Frames added to the running period (urgent commands that arrived after
 * the period plan): same entry format, scheduled from that period start. */
static void on_add(int gi, const uint8_t *m, size_t n)
{
    if (n >= 7) {
        queue_entries(gi, m, n, 5, gws[gi].period_start_ms);
    }
}

static void queue_entries(int gi, const uint8_t *m, size_t n, size_t p, uint32_t start)
{
    gwctx_t *g = &gws[gi];
    if (p + 2 > n) return;
    uint16_t cnt = slp_le16(m + p);
    p += 2;
    for (uint16_t i = 0; i < cnt; i++) {
        if (p + 7 > n || npend >= MAX_PENDING) break;
        pending_tx_t *t = &pend[npend];
        t->gw = gi;
        t->ch = m[p];
        t->slot = m[p + 1];
        t->ref = (uint32_t)m[p + 2] | (uint32_t)m[p + 3] << 8 | (uint32_t)m[p + 4] << 16 | (uint32_t)m[p + 5] << 24;
        t->len = m[p + 6];
        p += 7;
        if (p + t->len > n) break;
        memcpy(t->frame, m + p, t->len);
        p += t->len;
        t->due_ms = start + (uint32_t)t->slot * (uint32_t)g->slot_ms;
        npend++;
    }
}

static void on_gw_msg(int gi, const uint8_t *m, size_t n, const struct sockaddr_in *from)
{
    gwctx_t *g = &gws[gi];
    g->peer = *from;
    g->have_peer = true;
    switch (m[0]) {
    case SIM_MSG_HELLO:
        on_hello(gi, m, n);
        break;
    case SIM_MSG_PERIOD:
        on_period(gi, m, n);
        break;
    case SIM_MSG_ADD:
        on_add(gi, m, n);
        break;
    case SIM_MSG_TXNOW:
        if (n >= 6 && 6u + m[5] <= n) {
            deliver_now(gi, m + 6, m[5]);
        }
        break;
    default:
        break;
    }
}

/* ---- control port ---- */
static vlock_t *find_lock(const char *s)
{
    if (strlen(s) <= 5) { /* room name, e.g. 1203 */
        int v = atoi(s);
        int f = v / 100, r = v % 100;
        for (int k = 0; k < nlocks; k++) {
            if (locks[k].floor == f && locks[k].room == r) return &locks[k];
        }
        return NULL;
    }
    unsigned long long e = strtoull(s, NULL, 16);
    for (int k = 0; k < nlocks; k++) {
        if (locks[k].eui == e) return &locks[k];
    }
    return NULL;
}

static int parse_hex(const char *s, uint8_t *out, int cap)
{
    int n = 0;
    while (s[0] && s[1] && n < cap) {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1) break;
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static void reply(const struct sockaddr_in *to, const char *s, size_t n)
{
    sendto(ctrl_fd, s, n, 0, (const struct sockaddr *)to, sizeof(*to));
}

static size_t snapshot(char *out, size_t cap)
{
    uint32_t t = now_ms();
    size_t n = (size_t)snprintf(out, cap, "SNAP %d %d %d %d %d\n", nlocks, building, floors, rooms, loss_pct);
    for (int k = 0; k < nlocks && n + 96 < cap; k++) {
        vlock_t *lk = &locks[k];
        uint32_t age = lk->hw.ind_calls ? t - lk->hw.last_ind_ms : 999999;
        n += (size_t)snprintf(out + n, cap - n, "%016llx %d %u %d %d %u %u %d %d %u %d %d %u\n",
                              (unsigned long long)lk->eui, (int)lk->app.core.state, lk->hw.battery_mv,
                              lk->rf_on, lk->joined, lk->app.creds.count, lk->app.batt_level,
                              lk->hw.clutch_engaged, (int)lk->hw.last_ind, age, lk->mac.channel,
                              lk->mac.my_slot, lk->mac.short_addr);
    }
    return n;
}

static void on_ctrl(char *line, const struct sockaddr_in *from)
{
    static char out[65000];
    char *nl = strpbrk(line, "\r\n");
    if (nl) *nl = 0;
    char cmd[16] = "", a1[64] = "", a2[64] = "";
    sscanf(line, "%15s %63s %63s", cmd, a1, a2);

    if (!strcmp(cmd, "snapshot")) {
        reply(from, out, snapshot(out, sizeof(out)));
        return;
    }
    if (!strcmp(cmd, "subscribe")) {
        log_peer = *from;
        have_log_peer = true;
        reply(from, "OK", 2);
        return;
    }
    if (!strcmp(cmd, "loss")) {
        loss_pct = atoi(a1);
        vlog(NULL, "空口丢包率设为 %d%%", loss_pct);
        reply(from, "OK", 2);
        return;
    }
    vlock_t *lk = find_lock(a1);
    if (!lk) {
        int n = snprintf(out, sizeof(out), "ERR unknown command or lock: %s", line);
        reply(from, out, (size_t)n);
        return;
    }
    hal_sim_cur = &lk->hw;
    int n = 0;
    if (!strcmp(cmd, "swipe")) {
        uint8_t uid[16];
        int ul = parse_hex(a2, uid, sizeof(uid));
        if (ul < 4) {
            n = snprintf(out, sizeof(out), "ERR uid");
        } else {
            slp_result_t r = lock_app_access(&lk->app, SLP_CRED_CARD_UID, uid, (uint8_t)ul);
            vlog(lk, "刷校园卡 UID %s -> %s, 门锁状态 %s", a2, r == SLP_RESULT_OK ? "开门" : result_name(r),
                 lock_state_name(lk->app.core.state));
            flush_events(lk);
            n = snprintf(out, sizeof(out), "OK %d %s", r, lock_state_name(lk->app.core.state));
        }
    } else if (!strcmp(cmd, "battery")) {
        lk->hw.battery_mv = (uint16_t)atoi(a2);
        slp_batt_level_t lv = lock_app_battery_check(&lk->app);
        vlog(lk, "电池电压变为 %u mV (等级 %s)", lk->hw.battery_mv,
             lv == SLP_BATT_OK ? "正常" : lv == SLP_BATT_LOW ? "低电量" : "严重低电");
        flush_events(lk);
        n = snprintf(out, sizeof(out), "OK %d", lv);
    } else if (!strcmp(cmd, "rf")) {
        lk->rf_on = strcmp(a2, "off") != 0;
        vlog(lk, lk->rf_on ? "射频恢复 (锁重新在线)" : "射频被遮挡 (锁离线，收不到任何指令)");
        if (lk->rf_on && lk->joined) {
            /* Back in range: the lock re-syncs on the next beacon and
             * announces itself with a heartbeat, so the platform knows it is
             * reachable again (and re-sends parked commands). */
            uint8_t m[64];
            size_t hn = lock_app_heartbeat(&lk->app, m, sizeof(m));
            if (hn) lock_uplink(lk, m, hn);
            flush_events(lk);
            lk->hb_due_ms = now_ms() + (uint32_t)hb_s * 1000u;
        }
        n = snprintf(out, sizeof(out), "OK %d", lk->rf_on);
    } else if (!strcmp(cmd, "dropack")) {
        lk->drop_acks = atoi(a2);
        n = snprintf(out, sizeof(out), "OK");
    } else if (!strcmp(cmd, "jam")) {
        lk->hw.clutch_fail_next = atoi(a2);
        vlog(lk, "(模拟) 电机卡死，接下来 %d 次驱动失败", lk->hw.clutch_fail_next);
        n = snprintf(out, sizeof(out), "OK");
    } else if (!strcmp(cmd, "replay") || !strcmp(cmd, "tamper")) {
        if (lk->captured_len == 0) {
            n = snprintf(out, sizeof(out), "ERR nothing captured yet");
        } else {
            uint8_t f[SLP_MAX_FRAME_LEN];
            size_t fl = lk->captured_len;
            memcpy(f, lk->captured, fl);
            if (cmd[0] == 't') {
                f[fl - 4] ^= 0x5A; /* flip ciphertext/MIC bits, fix the CRC like a real attacker */
                uint16_t crc = slp_crc16(f, fl - 2);
                f[fl - 2] = (uint8_t)crc;
                f[fl - 1] = (uint8_t)(crc >> 8);
                vlog(lk, "攻击者: 篡改截获的帧并重发");
            } else {
                vlog(lk, "攻击者: 原样重放截获的上一帧 (%zu 字节)", fl);
            }
            lock_rx(lk, f, fl, true);
            n = snprintf(out, sizeof(out), "OK state %s", lock_state_name(lk->app.core.state));
        }
    } else {
        n = snprintf(out, sizeof(out), "ERR unknown command %s", cmd);
    }
    reply(from, out, (size_t)n);
}

/* ---- periodic work ---- */
static void tick(void)
{
    uint32_t t = now_ms();
    for (int k = 0; k < nlocks; k++) {
        vlock_t *lk = &locks[k];
        hal_sim_cur = &lk->hw;
        lock_core_tick(&lk->app.core);
        if (lk->app.core.state != lk->last_state) {
            if (lk->last_state == LOCK_ST_UNLOCKED && lk->app.core.state == LOCK_ST_LOCKED) {
                vlog(lk, "开门时间到，自动落锁");
            }
            lk->last_state = lk->app.core.state;
        }
        if (!lk->rf_on || !gws[lk->gw].have_peer) {
            continue;
        }
        if (!lk->joined && (int32_t)(t - lk->join_due_ms) >= 0) {
            send_join_req(lk);
            lk->join_due_ms = t + 3000 + (uint32_t)(rand() % 2000); /* retry if no accept */
        }
        if (lk->joined && (int32_t)(t - lk->hb_due_ms) >= 0) {
            uint8_t m[64];
            size_t n = lock_app_heartbeat(&lk->app, m, sizeof(m));
            if (n) lock_uplink(lk, m, n);
            flush_events(lk);
            lk->hb_due_ms = t + (uint32_t)hb_s * 1000u;
        }
        lock_mac_check_sync(&lk->mac, t);
    }
}

static int udp_bind(int port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, bind_host, &a.sin_addr);
    int big = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        fprintf(stderr, "bind %s:%d: %s\n", bind_host, port, strerror(errno));
        exit(1);
    }
    return fd;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: locksim [--building N] [--floors N] [--rooms N] [--floors-per-gw N]\n"
            "               [--port 9100] [--ctrl 9200] [--bind 127.0.0.1] [--heartbeat-s 30]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (i + 1 >= argc) usage();
        const char *v = argv[++i];
        if (!strcmp(a, "--building")) building = atoi(v);
        else if (!strcmp(a, "--floors")) floors = atoi(v);
        else if (!strcmp(a, "--rooms")) rooms = atoi(v);
        else if (!strcmp(a, "--floors-per-gw")) floors_per_gw = atoi(v);
        else if (!strcmp(a, "--port")) base_port = atoi(v);
        else if (!strcmp(a, "--ctrl")) ctrl_port = atoi(v);
        else if (!strcmp(a, "--bind")) bind_host = v;
        else if (!strcmp(a, "--heartbeat-s")) hb_s = atoi(v);
        else usage();
    }
    if (floors < 1 || rooms < 1 || rooms > 99 || floors > 99 || floors_per_gw < 1 || hb_s < 1) usage();
    nlocks = floors * rooms;
    ngw = (floors + floors_per_gw - 1) / floors_per_gw;
    if (nlocks > MAX_LOCKS || ngw > MAX_GW) {
        fprintf(stderr, "too many locks/gateways\n");
        return 2;
    }
    srand((unsigned)time(NULL));
    locks = calloc((size_t)nlocks, sizeof(vlock_t));
    if (!locks) return 1;

    uint8_t key[16];
    for (int g = 0; g < ngw; g++) {
        gws[g].eui = SIM_GW_EUI(building, g + 1);
        gws[g].pan = SIM_PAN(building, g + 1);
        gws[g].port = base_port + g;
        gws[g].fd = udp_bind(gws[g].port);
        gws[g].first = -1;
        sim_derive_key(SIM_TAG_GRP_LINK, gws[g].eui, key);
        slp_host_key_set(H_GRP_LINK(g), key);
        sim_derive_key(SIM_TAG_GRP_APP, gws[g].eui, key);
        slp_host_key_set(H_GRP_APP(g), key);
    }
    /* Counters must never repeat across simulator restarts (real locks
     * persist a counter block in NVM3); seed them from the wall clock. */
    uint32_t seed_ctr = (uint32_t)(time(NULL) - 1700000000) * 64u;
    for (int f = 1, k = 0; f <= floors; f++) {
        for (int r = 1; r <= rooms; r++, k++) {
            vlock_t *lk = &locks[k];
            lk->eui = SIM_LOCK_EUI(building, f, r);
            lk->floor = f;
            lk->room = r;
            lk->gw = (f - 1) / floors_per_gw;
            lk->rf_on = true;
            lk->hw.battery_mv = (uint16_t)(5600 + rand() % 400);
            hal_sim_cur = &lk->hw;
            lock_app_init(&lk->app);
            slp_txctr_restore(&lk->txc, seed_ctr);
            lk->upseq = seed_ctr;
            sim_derive_key(SIM_TAG_LINK, lk->eui, key);
            slp_host_key_set(H_LINK(k), key);
            sim_derive_key(SIM_TAG_APP, lk->eui, key);
            slp_host_key_set(H_APP(k), key);
            gwctx_t *g = &gws[lk->gw];
            if (g->first < 0) g->first = k;
            g->count++;
        }
    }
    ctrl_fd = udp_bind(ctrl_port);
    printf("locksim: building %d, %d floors x %d rooms = %d locks, %d gateways (UDP %d..%d), control UDP %d\n",
           building, floors, rooms, nlocks, ngw, base_port, base_port + ngw - 1, ctrl_port);
    fflush(stdout);

    struct pollfd pfd[MAX_GW + 1];
    uint32_t next_tick = now_ms();
    static uint8_t buf[65536];
    for (;;) {
        uint32_t t = now_ms();
        int timeout = (int)(next_tick - t);
        for (int i = 0; i < npend; i++) {
            int d = (int)(pend[i].due_ms - t);
            if (d < timeout) timeout = d;
        }
        if (timeout < 0) timeout = 0;
        for (int g = 0; g < ngw; g++) {
            pfd[g].fd = gws[g].fd;
            pfd[g].events = POLLIN;
        }
        pfd[ngw].fd = ctrl_fd;
        pfd[ngw].events = POLLIN;
        poll(pfd, (nfds_t)ngw + 1, timeout);

        for (int g = 0; g < ngw; g++) {
            if (!(pfd[g].revents & POLLIN)) continue;
            for (;;) {
                struct sockaddr_in from;
                socklen_t fl = sizeof(from);
                ssize_t n = recvfrom(gws[g].fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
                if (n <= 0) break;
                on_gw_msg(g, buf, (size_t)n, &from);
            }
        }
        if (pfd[ngw].revents & POLLIN) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            ssize_t n = recvfrom(ctrl_fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
            if (n > 0) {
                buf[n] = 0;
                on_ctrl((char *)buf, &from);
            }
        }

        t = now_ms();
        for (int i = 0; i < npend;) {
            if ((int32_t)(t - pend[i].due_ms) >= 0) {
                pending_tx_t tx = pend[i];
                pend[i] = pend[--npend];
                deliver_slot(&tx);
            } else {
                i++;
            }
        }
        if ((int32_t)(t - next_tick) >= 0) {
            tick();
            next_tick = t + 50;
        }
    }
}
