#include "cfw_context.h"
#include "protobuf.h"

/*
 * Damage settings extension — Damage FIRMWARE.md §0 and §3 (draft, Phase 1).
 *
 * Context: this repo is the Damage build of the G2 custom firmware for Adam's own
 * glasses (../DAMAGE.md). This file adds three small things to the sid-0x09 settings
 * channel the upstream patches already hook; it adds no new patch site.
 *
 *   field 110 DamageCaps     appended to every settings READ response:
 *                            { 1: "DMG", 2: contract, 3: features }
 *   field 112 DamageControl  a request body ['D','M',1,op,argLo,argHi]:
 *                            op 1 TELEMETRY (arg = request id), 2 FLAGS_SET (arg =
 *                            the complete flag set), 3 FLAGS_CLEAR
 *   field 111 DamageTelemetry the reply to every op, sent on the settings channel
 *
 * Flags: only bit 15 (PROBE, no behaviour) is implemented; a FLAGS_SET naming any
 * other bit changes nothing and records status 2. Flags clear wherever the texture
 * cache is released (lease expiry, FB_RELEASE, a fresh acquire, mode 11).
 *
 * The telemetry walk over the heap arenas is the same validated, lock-free read the
 * diagnostic overlay uses (debug.c); an arena that does not validate is omitted.
 */

#define DMG_CAPS_FIELD        110u
#define DMG_TELEMETRY_FIELD   111u
#define DMG_CONTRACT          1u
#define DMG_FEATURE_TELEMETRY (1u << 0)
#define DMG_FEATURE_FLAGS     (1u << 1)
#define DMG_FLAG_PROBE        (1u << 15)
#define DMG_FLAGS_IMPLEMENTED (DMG_FLAG_PROBE)
#define DMG_OP_TELEMETRY      1u
#define DMG_OP_FLAGS_SET      2u
#define DMG_OP_FLAGS_CLEAR    3u
#define DMG_STATUS_OK         0u
#define DMG_STATUS_MALFORMED  1u
#define DMG_STATUS_UNSUPPORTED 2u

/* The stock KV store's boot counter, read back and incremented at every start
 * (openCFW g2-service-kvdb-recovery.md, kvbooCount). Graded I until it is read on
 * glass across a reset. 0 = not known. */
#define DMG_BOOT_COUNT (*(volatile uint32_t *)0x20074988u)
/* The active ULED operations record (FUN_004c9f32 stores it): 0x0070afe4 A6N-G,
 * 0x0070b024 JBD4010, 0 before panel selection. */
#define DMG_PANEL_OPS  (*(volatile uint32_t *)0x20074530u)

void damage_clear_flags(customCfwContext *ctx) {
    if (ctx) ctx->dmg_flags = 0;
}

unsigned damage_append_caps(unsigned char *buf, unsigned len, unsigned capacity) {
    unsigned char body[16];
    unsigned n = 0;
    body[n++] = 0x0a; body[n++] = 3; body[n++] = 'D'; body[n++] = 'M'; body[n++] = 'G';
    body[n++] = 0x10; n += pb_write_varint(body + n, DMG_CONTRACT);
    body[n++] = 0x18; n += pb_write_varint(body + n, DMG_FEATURE_TELEMETRY | DMG_FEATURE_FLAGS);
    return pb_append_bytes_field(buf, len, capacity, DMG_CAPS_FIELD, body, n);
}

static unsigned damage_put_varint_field(unsigned char *p, unsigned field, unsigned value) {
    unsigned n = pb_write_varint(p, field << 3);
    return n + pb_write_varint(p + n, value);
}

static void damage_send_telemetry(customCfwContext *ctx, unsigned request_id) {
    unsigned char body[96];
    unsigned n = 0;
    int leased = cfw_fb_lease_active();          /* notices a lapse first: flags then read 0 */
    uint32_t tick = FW_MS_TICK;
    n += damage_put_varint_field(body + n, 1, request_id);
    n += damage_put_varint_field(body + n, 2, tick);
    n += damage_put_varint_field(body + n, 3, ctx->dmg_flags);
    n += damage_put_varint_field(body + n, 4, ctx->dmg_status);
    n += damage_put_varint_field(body + n, 5, ctx->last_worker_us);
    n += damage_put_varint_field(body + n, 6, ctx->last_present_us);
    uint32_t free13 = heap_object_free(0x20000354u, 0x2013be70u, 0x000cd000u);
    uint32_t free20 = *(volatile uint32_t *)0x20074abcu == 0x20208e70u
                          ? tlsf_arena_free(0x20208e70u, 0x00070800u) : TLSF_FREE_INVALID;
    uint32_t free27 = heap_object_free(0x20000338u, 0x20279670u, 0x0002cc00u);
    if (free13 != TLSF_FREE_INVALID) n += damage_put_varint_field(body + n, 7, free13 >> 10);
    if (free20 != TLSF_FREE_INVALID) n += damage_put_varint_field(body + n, 8, free20 >> 10);
    if (free27 != TLSF_FREE_INVALID) n += damage_put_varint_field(body + n, 9, free27 >> 10);
    uint32_t panel = DMG_PANEL_OPS;
    if (panel) n += damage_put_varint_field(body + n, 10, panel);
    unsigned diag = (unsigned)(ctx->f_reorder ? 1u : 0u) | (ctx->f_skip ? 2u : 0u) |
                    (ctx->f_dup ? 4u : 0u) | (ctx->f_snap_of ? 8u : 0u) |
                    ((cfw_alloc_diag() & 1u) ? 16u : 0u);
    n += damage_put_varint_field(body + n, 11, diag);
    uint32_t left = leased ? ctx->direct_lease_deadline - tick : 0u;
    n += damage_put_varint_field(body + n, 12, left);
    uint32_t boots = DMG_BOOT_COUNT;
    if (boots) n += damage_put_varint_field(body + n, 13, boots);
    n += damage_put_varint_field(body + n, 14, FW_SIDE_ID());

    /* G2SettingPackage{ 1: commandId 3, 2: magic 0, 111: body } */
    unsigned char *p = ctx->dmg_reply_buf;
    unsigned m = 0;
    p[m++] = 0x08; p[m++] = 0x03;
    p[m++] = 0x10; p[m++] = 0x00;
    m = pb_append_bytes_field(p, m, sizeof(ctx->dmg_reply_buf), DMG_TELEMETRY_FIELD, body, n);
    if (m == 4u) return;                         /* did not fit: nothing half-built is sent */
    ((send_fn)FW_SEND)(1, 9, p, m);
}

void damage_apply_control(const uint8_t *data, uint32_t len) {
    customCfwContext *ctx = getCustomCfwContext();
    if (!ctx) return;
    if (len != 6u || data[0] != 'D' || data[1] != 'M' || data[2] != 1u) {
        ctx->dmg_status = DMG_STATUS_MALFORMED;
        return;
    }
    unsigned op = data[3];
    unsigned arg = (unsigned)data[4] | ((unsigned)data[5] << 8);
    unsigned request_id = 0;
    if (op == DMG_OP_TELEMETRY) {
        request_id = arg;
        ctx->dmg_status = DMG_STATUS_OK;
    } else if (op == DMG_OP_FLAGS_SET) {
        if (arg & ~DMG_FLAGS_IMPLEMENTED) {
            ctx->dmg_status = DMG_STATUS_UNSUPPORTED;
        } else {
            ctx->dmg_flags = arg;
            ctx->dmg_status = DMG_STATUS_OK;
        }
    } else if (op == DMG_OP_FLAGS_CLEAR) {
        ctx->dmg_flags = 0;
        ctx->dmg_status = DMG_STATUS_OK;
    } else {
        ctx->dmg_status = DMG_STATUS_MALFORMED;
    }
    damage_send_telemetry(ctx, request_id);
}
