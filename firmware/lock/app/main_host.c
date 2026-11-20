/*
 * Host demo: runs the lock logic against the simulated HAL.
 * On EFR32 the equivalent entry point is port/efr32/app.c (TODO), where the
 * radio task, BLE stack and peripherals feed the same lock_app_* calls.
 */
#include <stdio.h>

#include "hal_host.h"
#include "lock_app.h"

static lock_app_t app;

int main(void)
{
    hal_host_reset();
    lock_app_init(&app);

    uint8_t req[64], rsp[64];
    slp_app_writer_t w;
    slp_app_hdr_t h = { .cmd = SLP_CMD_CRED_ADD, .msg_id = 1, .expires_at = 0 };
    const uint8_t uid[4] = { 0x8A, 0x3F, 0x11, 0x5C };
    slp_app_begin(&w, req, sizeof(req), &h);
    slp_app_put_u8(&w, SLP_TLV_CRED_TYPE, SLP_CRED_CARD_UID);
    slp_app_put(&w, SLP_TLV_CRED_ID, uid, sizeof(uid));
    size_t n = lock_app_handle_command(&app, req, w.len, rsp, sizeof(rsp));
    printf("CRED_ADD -> %zu byte response, state %s\n", n, lock_state_name(app.core.state));

    slp_result_t r = lock_app_access(&app, SLP_CRED_CARD_UID, uid, sizeof(uid));
    printf("card access -> result %d, state %s\n", r, lock_state_name(app.core.state));

    hal_host_advance(5000);
    lock_core_tick(&app.core);
    printf("after 5 s -> state %s\n", lock_state_name(app.core.state));
    return 0;
}
