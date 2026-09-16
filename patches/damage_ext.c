#include "cfw_context.h"
#include "protobuf.h"

/*
 * Damage settings extension — Damage FIRMWARE.md §0 and §3 (draft, Phase 1).
 *
 * Context: this repo is the Damage build of the G2 custom firmware for Adam's own
 * glasses (../DAMAGE.md). This file adds a small control channel to the sid-0x09
 * settings service the upstream patches already hook, a timing stamp around the
 * display task's panel refresh, a cache that can outlive a lease lapse, and a
 * self-test that runs drawing messages against scratch memory. Everything is off
 * until the phone arms it; a phone that is gone leaves stock behaviour behind when
 * its lease lapses.
 *
 *   field 110 DamageCaps     appended to every settings READ response:
 *                            { 1: "DMG", 2: contract, 3: features }
 *   field 112 DamageControl  a request body ['D','M',1,op,argLo,argHi]:
 *                            op 1 TELEMETRY (arg = request id), 2 FLAGS_SET (arg =
 *                            the complete flag set), 3 FLAGS_CLEAR, 4 CACHE_INFO
 *                            (arg = request id; the record also carries the cache CRC)
 *   field 111 DamageTelemetry the reply to every op, sent on the settings channel
 *   field 113 DamagePresented a notify after each panel transfer of a Damage frame
 *                            while flag bit 0 is armed: { 1 sequence, 2 worker µs,
 *                            3 copy µs, 4 transfer µs, 5 lens }
 *   mode 16 (image lane)     the self-test: [16][0] begin · [16][1][message] step ·
 *                            [16][2] end (below)
 *   modes 17-24 (image lane) the drawing contract v2 (damage_draw.c), behind flag bit 2 DRAW2
 *   op 5 CACHE_SIZE          the session's texture-cache size in KiB, set before the first
 *                            write: status 3 with no lease, 4 once the cache is allocated,
 *                            5 outside 64..160 KiB (modes 12/13/14 bound their records by the
 *                            first 64 KiB, so a smaller cache would put those bounds past its
 *                            end); field 18 reports the size the allocation took
 *
 * Phase 2 (contract 2, Damage FIRMWARE.md §4): a FLAGS_SET with no lease held is refused with
 * status 3 (Adam's ruling, 2026-09-15); every image-lane refusal records its mode byte, a
 * reason and the copy sequence in fields 23-25 (damage_draw.c); field 26 says whether the last
 * Damage transfer took the full refresh (0) or the JBD4010's per-row partial entry (1, mode 24);
 * the presented notify carries the same as its field 6. DamageCaps features bit 5 says the v2
 * drawing ops are present, bit 6 that the link edits (patch_compress.py) are in this image.
 *
 * Flags (bit 15 PROBE has no behaviour): bit 0 PRESENTED sends field 113 after each
 * direct present; bit 1 CACHE_KEEP keeps the texture cache across the next lease
 * lapse and the fresh acquire that follows it. A FLAGS_SET with no lease held is refused
 * with status 3 before anything else; one naming a bit this build does not implement
 * changes nothing and records status 2. Flags clear wherever the texture cache
 * would be released (lease expiry, FB_RELEASE, a fresh acquire, mode 11).
 *
 * The status (field 4 of the record) is a register: it holds the status recorded by
 * the last op that records one — FLAGS_SET, FLAGS_CLEAR, a malformed or unknown
 * request. TELEMETRY and CACHE_INFO record nothing, so a refusal the phone did not
 * see is still readable afterwards (FIRMWARE.md §1.2).
 *
 * Only the RIGHT lens can send: the stock senders FUN_00475b14 (responses) and
 * FUN_00475c1a (notifies) both call FUN_0046f258, which returns 1 on the left lens
 * (FUN_0045a568() == 2), and answer 8 without sending (0x00475bb6-0x00475c00, read
 * at instruction level 2026-09-14). The left lens still runs every op — its flags,
 * cache and self-test state follow the same writes — but nothing it computes reaches
 * the phone over its own link. Replies and notifies are therefore built on RIGHT only.
 *
 * The telemetry walk over the heap arenas is the same validated, bounded, lock-free
 * read the diagnostic overlay uses (malloc.c tlsf_arena_free: every size word is
 * range-checked before it is read); an arena that does not validate is omitted.
 *
 * The boot count (field 13) is not sent by this build. Stock reads `kvbooCount`
 * through the KV get into a stack temporary, adds one and writes it back
 * (FUN_004d96d8, 0x004d99de..0x004d9a38); no RAM word holds the value, so reading
 * it means calling the KV store, and that call has no precedent from this context
 * yet (Damage CLAIMS.md, 2026-09-14).
 *
 * The reply buffers are one per lens: the stock sender copies the body into its own
 * queue block before it returns (FUN_00475b14 -> FUN_0047564e).
 *
 * ---- The panel-transfer stamp (F1.3) ----------------------------------------------
 * The display task FUN_00473c44 handles a type-3 refresh as: the copy hook (bl at
 * 0x00473c8e, redirected by upstream to display_copy_hook), the gate give (0x00473c92),
 * a panel-on check, then `bl FUN_004ca564` at 0x00473ce4 with six arguments — the ULED
 * manager's refresh dispatcher, which calls the active panel record's async refresh
 * (+0x28): one fixed 153,602-byte transfer on both drivers (Damage CLAIMS.md,
 * 2026-09-13). damage_refresh_hook replaces that one bl. It runs for every stock
 * refresh from boot on, so it allocates nothing and reads only a validated context
 * (peekCustomCfwContext, the same read display_copy_hook makes on this task): with no
 * context, or when the frame being refreshed is not a Damage frame, it is the stock
 * call with its six arguments unchanged. After a direct copy it stamps the call with
 * the DWT cycle counter (debug.c) and, while PRESENTED is armed, sends field 113. The
 * copy hook clears the mark again on its stock-copy path, so a mark left by a direct
 * copy whose refresh the display task skipped (panel off, 0x00473cca) never stamps a
 * later stock refresh as a Damage frame's.
 *
 * ---- Cache-keep (F1.5) ----------------------------------------------------------------
 * Upstream frees the texture cache at four points: a lapse noticed by
 * cfw_fb_lease_active, FB_RELEASE, a fresh FB_ACQUIRE after a lapse, and mode 11
 * (settings_ext.c, zlib_glue.c). The first three now go through damage_lease_ended /
 * damage_lease_fresh_acquire: at the lapse or release the CACHE_KEEP flag, read before
 * the flags clear, is latched — once per lease (dmg_lease_settled), so a release that
 * follows a lapse already noticed cannot re-read the cleared flags and drop the latch;
 * the fresh acquire that follows consumes the latch and keeps or frees the cache. The
 * flags themselves clear at every release point whether the lapse was settled before it
 * or not. The phone re-arms the flag every session; a session that
 * does not re-arm it gets upstream's behaviour at the next lapse. Mode 11 (the
 * hand-back to stock) frees the cache regardless. dmg_cache_gen counts the
 * cache writes that changed it (texture_cache.c mode 12, damage_draw.c mode 19); CACHE_INFO
 * adds the CRC-32 of the whole allocated size (op 5's, 64 KiB unless asked otherwise) so the
 * phone can tell whether what it uploaded is still there.
 *
 * ---- The self-test (mode 16) -----------------------------------------------------------
 * [16][0] allocates and zeroes a scratch shadow (PANEL_BYTES from heap 13) while the
 * lease is held, under the same active mark a step runs under; [16][1][message] runs one drawing message — any shadow message a
 * batch could carry: 3/6/8/9/13/14/15, with mode 8's own rules inside a batch — through
 * the unchanged image_dispatch against the scratch shadow, with presents suppressed
 * (present_shadow returns while dmg_st_active) and the self-test's own fid ring and
 * sticky flags swapped in, so a vector cannot disturb the live frame or its diagnostics;
 * afterwards the scratch shadow's CRC-32 (zlib polynomial) is recorded with the step
 * count and whether the message was refused, for the telemetry record (fields 20-22).
 * Every step needs the lease (the check settles a lapse, which frees the scratch).
 * Modes 13/14/15 read the live texture cache and the built-in font; mode 12 (a live
 * cache write) and every non-drawing mode are refused, and so is a mode-3/6 message
 * too short for its own header (the normal path's BMP fallback, load_bmp_fast, refuses
 * while a step runs: the scratch is not a container and the live direct frame is not
 * the step's to drop). [16][2] frees the scratch; so does every lease release point.
 * Both lenses run every step; only RIGHT can report. Mode 23 inside a step takes the self-test's
 * save-under slots (damage_draw.c picks the set by the active mark; nothing is swapped), so a
 * release point on another task frees the live set at once and defers only the self-test's.
 */

#define DMG_CAPS_FIELD        110u
#define DMG_TELEMETRY_FIELD   111u
#define DMG_PRESENTED_FIELD   113u
#define DMG_CONTRACT          2u             /* 2: FLAGS_SET with no lease is refused (Phase 2) */
#define DMG_FEATURE_TELEMETRY (1u << 0)
#define DMG_FEATURE_FLAGS     (1u << 1)
#define DMG_FEATURE_PRESENTED (1u << 2)
#define DMG_FEATURE_CACHEKEEP (1u << 3)
#define DMG_FEATURE_SELFTEST  (1u << 4)
#define DMG_FEATURE_DRAW2     (1u << 5)      /* modes 17-24, op 5, fields 23-26 (damage_draw.c) */
#define DMG_FEATURE_LINK      (1u << 6)      /* the three link edits (patch_compress.py): LE 2M, 7.5 ms, no slow set */
#define DMG_FEATURES (DMG_FEATURE_TELEMETRY | DMG_FEATURE_FLAGS | DMG_FEATURE_PRESENTED | \
                      DMG_FEATURE_CACHEKEEP | DMG_FEATURE_SELFTEST | DMG_FEATURE_DRAW2 | DMG_FEATURE_LINK)
#define DMG_FLAG_PRESENTED    (1u << 0)
#define DMG_FLAG_CACHE_KEEP   (1u << 1)
#define DMG_FLAG_DRAW2        (1u << 2)      /* the v2 drawing ops answer */
#define DMG_FLAG_PROBE        (1u << 15)
#define DMG_FLAGS_IMPLEMENTED (DMG_FLAG_PRESENTED | DMG_FLAG_CACHE_KEEP | DMG_FLAG_DRAW2 | DMG_FLAG_PROBE)
#define DMG_OP_TELEMETRY      1u
#define DMG_OP_FLAGS_SET      2u
#define DMG_OP_FLAGS_CLEAR    3u
#define DMG_OP_CACHE_INFO     4u
#define DMG_OP_CACHE_SIZE     5u
#define DMG_STATUS_OK         0u
#define DMG_STATUS_MALFORMED  1u
#define DMG_STATUS_UNSUPPORTED 2u
#define DMG_STATUS_NO_LEASE   3u
#define DMG_STATUS_ALLOCATED  4u             /* CACHE_SIZE while the cache is up: the size stands */
#define DMG_STATUS_BUDGET     5u             /* CACHE_SIZE outside 64..160 KiB */
#define DMG_CACHE_BUDGET_KIB  160u           /* budget A (Damage FIRMWARE.md §4) */
#define DMG_CACHE_MIN_KIB     64u            /* modes 12/13/14 bound their records by the first 64 KiB */
#define DMG_ST_BEGIN          0u
#define DMG_ST_STEP           1u
#define DMG_ST_END            2u
#define DMG_ST_SHADOW_BYTES   PANEL_BYTES     /* the packed 640x480 panel (zlib_glue.c) */

/* The active ULED operations record (FUN_004c9f32 stores it): 0x0070afe4 A6N-G,
 * 0x0070b024 JBD4010, 0 before panel selection. */
#define DMG_PANEL_OPS  (*(volatile uint32_t *)0x20074530u)
#define DMG_PANEL_JBD4010 0x0070b024u
/* The record's +0x2C: the blocking per-row partial refresh (JBD4010 FUN_00592cb4, Damage
 * CLAIMS.md 2026-09-13): the same six arguments as the async +0x28 the dispatcher calls —
 * (x offset, y offset, x0, y0, x1, y1), the end coordinates inclusive and clamped to 639/479;
 * one transfer per row from y0 to y1, then the latch command and a 1 ms delay. Called through
 * the record, never by address, so an A6N-G pair (whose +0x2C moves no pixels) keeps the full
 * refresh: mode 24 checks the record first. */
typedef int (*panel_partial_fn)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

/* FUN_004ca564: the ULED manager's refresh dispatcher, `bl` target at 0x00473ce4
 * (r0-r3 plus two stack words; it stores the two stack words for the callback and
 * calls the active record's +0x28). Thumb bit set for the constant-pointer call. */
typedef int (*panel_refresh_fn)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
#define FW_PANEL_REFRESH ((panel_refresh_fn)0x004ca565u)

/* ---- CRC-32, zlib polynomial 0xEDB88320, reflected, init and final ~0 --------------
 * The same function zlib's crc32() computes, over any byte range; a 16-entry table
 * keeps the read-only data small (build.py places it after .text, -fropi). */
static const uint32_t damage_crc_table[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

static uint32_t damage_crc32(const uint8_t *p, uint32_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t b = p[i];
        crc = damage_crc_table[(crc ^ b) & 15u] ^ (crc >> 4);
        crc = damage_crc_table[(crc ^ (b >> 4)) & 15u] ^ (crc >> 4);
    }
    return ~crc;
}

/* ---- flags and the lease's release points --------------------------------------------- */

void damage_clear_flags(customCfwContext *ctx) {
    if (ctx) ctx->dmg_flags = 0;
}

/* The scratch is used by the EvenHub task (a step) and released from wherever a
 * lease release point runs — the settings task (FB_RELEASE, a fresh acquire) or the
 * input thread (a lapse noticed by cfw_fb_lease_active). A release that lands while a
 * step runs is deferred to the step's epilogue instead of freeing under it. The fields
 * are volatile (cfw_context.h) so the step's mark-then-read and this check-then-free
 * keep their program order; the window between a release's check and a step's mark is
 * the same one the installed firmware has for its texture cache and is left as it is. */
/* The free itself, with no mark to consult: called by the release below when no step is running,
 * and by the step's own epilogue while its mark is still up. The scratch pointer is claimed with
 * an exchange, so two tasks reaching a release point together cannot both take it and hand the
 * same heap-13 block back twice (2026-09-15, the third review). */
static void damage_self_test_free(customCfwContext *ctx) {
    uint8_t *shadow = __atomic_exchange_n(&ctx->dmg_st_shadow, (uint8_t *)0, __ATOMIC_SEQ_CST);
    if (shadow) cfw_heap13_free(shadow);
    damage_slots_free(ctx->dmg_st_slots, &ctx->dmg_st_slot_bytes);   /* the self-test's save-under slots */
}

static void damage_self_test_release(customCfwContext *ctx) {
    if (ctx == 0) return;
    if (ctx->dmg_st_active) { ctx->dmg_st_free_pending = 1; return; }
    damage_self_test_free(ctx);
}

/* The live save-under slots and the size asked for, at every release point. A step never
 * touches the live slots (mode 23 takes the self-test's set while one runs), but a live mode 23
 * on the image worker does, and this runs on the settings task or the input thread: a free that
 * lands while the worker holds the slots is deferred to the worker's epilogue, as a step's is
 * for the scratch (2026-09-15, second review). The size asked for goes whether the cache went
 * or was kept by CACHE_KEEP. The panel is stock's again after a release, so the next Damage
 * frame goes whole rather than through a partial refresh (mode 24). */
static void damage_live_slots_release(customCfwContext *ctx) {
    if (ctx->dmg_slots_active) ctx->dmg_slots_free_pending = 1;
    else damage_slots_free(ctx->dmg_slots, &ctx->dmg_slot_bytes);
    ctx->dmg_cache_req = 0;
    ctx->dmg_panel_stale = 1;
}

/* The end of a begin or a step: the active mark drops, and a release that landed
 * from another task meanwhile (deferred above) is honoured now — the scratch and the
 * self-test's slots. Returns 1 if one had. */
static int damage_self_test_settle(customCfwContext *ctx) {
    /* the deferred free runs while the mark is still UP, so a release that lands during it is
     * deferred again rather than freeing the same buffer twice (2026-09-15, the third review) */
    int had = 0;
    while (ctx->dmg_st_free_pending) {
        ctx->dmg_st_free_pending = 0;
        damage_self_test_free(ctx);             /* still under the mark: a release meanwhile defers again */
        had = 1;
    }
    ctx->dmg_st_active = 0;
    return had;
}

/* A lapse noticed by cfw_fb_lease_active, or an FB_RELEASE: settle what the flags
 * said before they clear. The cache is kept only when CACHE_KEEP was armed, and the
 * decision is latched for the fresh acquire that follows. Settled ONCE per lease:
 * a release after a lapse already noticed (the flags are clear by then) must not
 * read the cleared flags as "not armed" and drop the latch. */
void damage_lease_ended(customCfwContext *ctx) {
    if (ctx == 0) return;
    damage_self_test_release(ctx);
    if (ctx->dmg_lease_settled) {
        /* Settled already (a lapse noticed before this release): the latch stands, but
         * the flags still clear — a FLAGS_SET taken since that lapse would otherwise
         * survive the FB_RELEASE (2026-09-14, second review). */
        ctx->dmg_flags = 0;
        return;
    }
    ctx->dmg_lease_settled = 1;
    ctx->dmg_cache_keep_latched = (ctx->dmg_flags & DMG_FLAG_CACHE_KEEP) ? 1u : 0u;
    if (!ctx->dmg_cache_keep_latched) cfw_texture_cache_release(ctx);
    damage_live_slots_release(ctx);                 /* save-under: freed at every release point */
    ctx->dmg_flags = 0;
}

/* A fresh FB_ACQUIRE (no lease, or one that lapsed unnoticed): a lapse nobody
 * settled is settled first, then the latch decides whether the cache carries over.
 * The flags are already clear either way; the next lease starts unsettled. */
void damage_lease_fresh_acquire(customCfwContext *ctx) {
    if (ctx == 0) return;
    if (!ctx->dmg_lease_settled) damage_lease_ended(ctx);   /* an unsettled lapse */
    damage_self_test_release(ctx);
    if (!ctx->dmg_cache_keep_latched) cfw_texture_cache_release(ctx);
    damage_live_slots_release(ctx);
    ctx->dmg_cache_keep_latched = 0;
    ctx->dmg_lease_settled = 0;
    ctx->dmg_flags = 0;
}

/* Mode 11: the session hands the display back to stock. Nothing Damage owns survives. */
void damage_session_cleanup(customCfwContext *ctx) {
    if (ctx == 0) return;
    damage_self_test_release(ctx);
    damage_live_slots_release(ctx);
    ctx->dmg_cache_keep_latched = 0;
    ctx->dmg_lease_settled = 0;
    ctx->dmg_flags = 0;
}

/* ---- the capability field and the telemetry record ------------------------------------ */

unsigned damage_append_caps(unsigned char *buf, unsigned len, unsigned capacity) {
    unsigned char body[16];
    unsigned n = 0;
    body[n++] = 0x0a; body[n++] = 3; body[n++] = 'D'; body[n++] = 'M'; body[n++] = 'G';
    body[n++] = 0x10; n += pb_write_varint(body + n, DMG_CONTRACT);
    body[n++] = 0x18; n += pb_write_varint(body + n, DMG_FEATURES);
    return pb_append_bytes_field(buf, len, capacity, DMG_CAPS_FIELD, body, n);
}

static unsigned damage_put_varint_field(unsigned char *p, unsigned field, unsigned value) {
    unsigned n = pb_write_varint(p, field << 3);
    return n + pb_write_varint(p + n, value);
}

/* G2SettingPackage{ 1: commandId 3, 2: magic 0, <field>: body } through the stock
 * response sender, from RIGHT only (the sender's own lens rule, above). */
static void damage_send_package(unsigned char *buf, unsigned capacity, unsigned field,
                                const unsigned char *body, unsigned n) {
    if (FW_SIDE_ID() != 1u) return;
    unsigned m = 0;
    buf[m++] = 0x08; buf[m++] = 0x03;
    buf[m++] = 0x10; buf[m++] = 0x00;
    m = pb_append_bytes_field(buf, m, capacity, field, body, n);
    if (m == 4u) return;                         /* did not fit: nothing half-built is sent */
    ((send_fn)FW_SEND)(1, 9, buf, m);
}

static void damage_send_telemetry(customCfwContext *ctx, unsigned request_id, int with_cache_crc) {
    unsigned char body[176];          /* 25 fields, worst case 161 B if every value were 5 bytes */
    unsigned n = 0;
    int leased = cfw_fb_lease_active();          /* notices a lapse first: flags then read 0 */
    uint32_t tick = FW_MS_TICK;
    /* The display task writes both of these at the end of a refresh: take them together, so a
     * refresh between the two fields cannot pair one frame's microseconds with the next
     * frame's path (2026-09-15, second review). */
    uint32_t transfer_us = ctx->dmg_last_transfer_us;
    uint32_t last_path = ctx->dmg_last_path;
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
    /* the deadline can pass between the lease check and the tick read, and an unsigned subtract
     * would then report ~4.29e9 ticks left (2026-09-15, the third review) */
    int32_t remain = (int32_t)(ctx->direct_lease_deadline - tick);
    uint32_t left = (leased && remain > 0) ? (uint32_t)remain : 0u;
    n += damage_put_varint_field(body + n, 12, left);
    /* field 13 (boot count): not sent — see the header comment */
    n += damage_put_varint_field(body + n, 14, FW_SIDE_ID());
    n += damage_put_varint_field(body + n, 15, transfer_us);
    n += damage_put_varint_field(body + n, 16, ctx->dmg_present_seq);
    n += damage_put_varint_field(body + n, 17, ctx->dmg_cache_gen);
    if (ctx->texture_cache) {
        uint32_t size = damage_cache_size(ctx);
        n += damage_put_varint_field(body + n, 18, size);
        if (with_cache_crc)
            n += damage_put_varint_field(body + n, 19, damage_crc32(ctx->texture_cache, size));
    }
    n += damage_put_varint_field(body + n, 20, ctx->dmg_st_seq);
    if (ctx->dmg_st_seq) {
        n += damage_put_varint_field(body + n, 21, ctx->dmg_st_refused);
        n += damage_put_varint_field(body + n, 22, ctx->dmg_st_crc);
    }
    /* the last image-lane refusal (damage_draw.c), read whole: the image lane may be writing it,
     * so the fields are taken from an even write count that did not move meanwhile (a few tries;
     * a record still being rewritten after them is left out of this reply, not sent mixed) */
    for (unsigned tries = 0; tries < 4u; tries++) {
        uint32_t gen = ctx->dmg_ref_gen;
        if (gen & 1u) continue;
        uint8_t seen = ctx->dmg_ref_seen, mode = ctx->dmg_ref_mode, reason = ctx->dmg_ref_reason;
        uint32_t seq = ctx->dmg_ref_seq;
        if (gen != ctx->dmg_ref_gen) continue;
        if (seen) {
            n += damage_put_varint_field(body + n, 23, mode);
            n += damage_put_varint_field(body + n, 24, reason);
            n += damage_put_varint_field(body + n, 25, seq);
        }
        break;
    }
    n += damage_put_varint_field(body + n, 26, last_path);
    damage_send_package(ctx->dmg_reply_buf, sizeof(ctx->dmg_reply_buf), DMG_TELEMETRY_FIELD, body, n);
}

void damage_apply_control(const uint8_t *data, uint32_t len) {
    customCfwContext *ctx = getCustomCfwContext();
    if (!ctx) return;
    if (len != 6u || data[0] != 'D' || data[1] != 'M' || data[2] != 1u) {
        ctx->dmg_status = DMG_STATUS_MALFORMED;  /* recorded, not answered */
        return;
    }
    unsigned op = data[3];
    unsigned arg = (unsigned)data[4] | ((unsigned)data[5] << 8);
    unsigned request_id = 0;
    int with_cache_crc = 0;
    if (op == DMG_OP_TELEMETRY) {
        request_id = arg;                        /* records no status: field 4 is the register */
    } else if (op == DMG_OP_CACHE_INFO) {
        request_id = arg;                        /* records no status either */
        with_cache_crc = 1;
    } else if (op == DMG_OP_FLAGS_SET) {
        if (!cfw_fb_lease_active()) {               /* contract 2: nothing is armed without the lease */
            ctx->dmg_status = DMG_STATUS_NO_LEASE;
        } else if (arg & ~DMG_FLAGS_IMPLEMENTED) {
            ctx->dmg_status = DMG_STATUS_UNSUPPORTED;
        } else {
            ctx->dmg_flags = arg;
            ctx->dmg_status = DMG_STATUS_OK;
        }
    } else if (op == DMG_OP_CACHE_SIZE) {
        /* the size the cache is allocated at by its first write; the v1 window stays 64 KiB */
        if (!cfw_fb_lease_active()) ctx->dmg_status = DMG_STATUS_NO_LEASE;
        else if (arg < DMG_CACHE_MIN_KIB || arg > DMG_CACHE_BUDGET_KIB) ctx->dmg_status = DMG_STATUS_BUDGET;
        else if (ctx->texture_cache) ctx->dmg_status = DMG_STATUS_ALLOCATED;
        else { ctx->dmg_cache_req = arg * 1024u; ctx->dmg_status = DMG_STATUS_OK; }   /* a request only: the allocation owns the bound */
    } else if (op == DMG_OP_FLAGS_CLEAR) {
        ctx->dmg_flags = 0;
        ctx->dmg_status = DMG_STATUS_OK;
    } else {
        ctx->dmg_status = DMG_STATUS_MALFORMED;  /* unknown op: recorded and answered */
    }
    damage_send_telemetry(ctx, request_id, with_cache_crc);
}

/* ---- the panel-transfer stamp and the presented notify (F1.3) ------------------------- */

static void damage_send_presented(customCfwContext *ctx) {
    unsigned char body[40];
    unsigned n = 0;
    n += damage_put_varint_field(body + n, 1, ctx->dmg_present_seq);
    n += damage_put_varint_field(body + n, 2, ctx->last_worker_us);
    n += damage_put_varint_field(body + n, 3, ctx->last_present_us);
    n += damage_put_varint_field(body + n, 4, ctx->dmg_last_transfer_us);
    n += damage_put_varint_field(body + n, 5, FW_SIDE_ID());
    n += damage_put_varint_field(body + n, 6, ctx->dmg_last_path);
    damage_send_package(ctx->dmg_notify_buf, sizeof(ctx->dmg_notify_buf), DMG_PRESENTED_FIELD, body, n);
}

/* Replaces `bl FUN_004ca564` at 0x00473ce4 (the display task's type-3 refresh). See
 * the header comment: a pass-through for every refresh that did not follow a direct
 * copy, the stamp otherwise. display_copy_hook sets dmg_direct_presented after each
 * direct copy; this consumes it. Never allocates; never runs the DWT calibration
 * (the copy hook already did, for this same frame). */
int damage_refresh_hook(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (ctx == 0 || !ctx->dmg_direct_presented) return FW_PANEL_REFRESH(a, b, c, d, e, f);
    ctx->dmg_direct_presented = 0;
    /* Mode 24 (F1.7): the hint the copy hook latched for this frame sends rows y0..y1 through
     * the JBD4010's per-row partial entry; any other panel, or no hint, the full refresh. The
     * arguments are a Damage job's (0, 0, 0, y0, 640, y1) whatever job carries this refresh: a
     * stock job queues 576x288, and the entry sizes every row from x1 (2026-09-15 review). */
    uint32_t ops = DMG_PANEL_OPS;
    int partial = ctx->dmg_hint_r_on && ops == DMG_PANEL_JBD4010;
    uint32_t y0 = ctx->dmg_hint_r_y0, y1 = ctx->dmg_hint_r_y1;
    ctx->dmg_hint_r_on = 0;
    uint32_t t;
    cfw_time_start(&t);
    int r;
    if (partial) {
        panel_partial_fn part = *(panel_partial_fn *)(uintptr_t)(ops + 0x2cu);
        (void)a; (void)b; (void)c; (void)e;
        r = part(0u, 0u, 0u, y0, PANEL_W, y1);
    } else {
        r = FW_PANEL_REFRESH(a, b, c, d, e, f);
    }
    ctx->dmg_last_transfer_us = cfw_time_end(&t);
    ctx->dmg_last_path = partial ? 1u : 0u;
    if (ctx->dmg_flags & DMG_FLAG_PRESENTED) damage_send_presented(ctx);
    return r;
}

/* ---- the self-test (mode 16) ------------------------------------------------------------- */

static void damage_swap_diag(customCfwContext *ctx, damage_diag_state *s) {
    damage_diag_state t;
    t.last_fid = ctx->last_fid; t.high_fid = ctx->high_fid;
    t.diag_seen = ctx->diag_seen; t.fid_resync = ctx->fid_resync;
    t.f_reorder = ctx->f_reorder; t.f_skip = ctx->f_skip; t.f_dup = ctx->f_dup;
    t.recent_pos = ctx->recent_pos;
    for (uint32_t i = 0; i < CFW_FID_RING; i++) t.recent_fids[i] = ctx->recent_fids[i];
    ctx->last_fid = s->last_fid; ctx->high_fid = s->high_fid;
    ctx->diag_seen = s->diag_seen; ctx->fid_resync = s->fid_resync;
    ctx->f_reorder = s->f_reorder; ctx->f_skip = s->f_skip; ctx->f_dup = s->f_dup;
    ctx->recent_pos = s->recent_pos;
    for (uint32_t i = 0; i < CFW_FID_RING; i++) ctx->recent_fids[i] = s->recent_fids[i];
    *s = t;
}

static void damage_diag_reset(damage_diag_state *s) {
    s->last_fid = 0; s->high_fid = 0;
    s->diag_seen = 0; s->fid_resync = 0;
    s->f_reorder = 0; s->f_skip = 0; s->f_dup = 0;
    s->recent_pos = 0;
    for (uint32_t i = 0; i < CFW_FID_RING; i++) s->recent_fids[i] = 0xffffu;   /* the context's sentinel */
}

/* [16][sub][...]: see the header comment. Returns the sub-message's own return for a
 * step, 0 for a begin/end that took effect, -1 for anything refused. */
int damage_self_test(const uint8_t *src, uint32_t srclen) {
    customCfwContext *ctx = getCustomCfwContext();
    if (ctx == 0 || src == 0) return -1;
    if (srclen < 2u) { damage_refuse(src, DMG_REF_LENGTH); return -1; }
    unsigned sub = src[1];
    if (sub == DMG_ST_BEGIN) {
        if (!cfw_fb_lease_active()) { damage_refuse(src, DMG_REF_NO_LEASE); return -1; }
        /* The scratch is allocated and zeroed under the active mark, as a step runs
         * under it: a release from another task in that time is deferred to the settle
         * below instead of freeing the scratch while it is being zeroed (2026-09-14,
         * second review). A begin the lease ended during is refused, the scratch freed. */
        ctx->dmg_st_active = 1;
        uint8_t *shadow = ctx->dmg_st_shadow;
        if (shadow == 0) {
            shadow = (uint8_t *)cfw_heap13_malloc(DMG_ST_SHADOW_BYTES);
            if (shadow == 0) { damage_self_test_settle(ctx); damage_refuse(src, DMG_REF_NO_MEMORY); return -1; }
            ctx->dmg_st_shadow = shadow;
        }
        bzero(shadow, DMG_ST_SHADOW_BYTES);
        ctx->dmg_st_seq = 0;
        ctx->dmg_st_refused = 0;
        ctx->dmg_st_crc = 0;
        damage_diag_reset(&ctx->dmg_st_diag);
        damage_slots_free(ctx->dmg_st_slots, &ctx->dmg_st_slot_bytes);   /* a begin starts with empty slots */
        if (damage_self_test_settle(ctx)) { damage_refuse(src, DMG_REF_SCRATCH); return -1; }   /* a release ended it */
        return 0;
    }
    if (sub == DMG_ST_END) {
        damage_self_test_release(ctx);
        return 0;
    }
    if (sub != DMG_ST_STEP) { damage_refuse(src, DMG_REF_VALUE); return -1; }
    if (srclen < 3u) { damage_refuse(src, DMG_REF_LENGTH); return -1; }
    if (!cfw_fb_lease_active()) { damage_refuse(src, DMG_REF_NO_LEASE); return -1; }   /* a lapse is settled here: the scratch goes with it */
    /* From here to the CRC a release from another task is deferred (damage_self_test_release);
     * the scratch pointer is read once, under that guard. */
    ctx->dmg_st_active = 1;
    uint8_t *scratch = ctx->dmg_st_shadow;
    if (scratch == 0) { damage_self_test_settle(ctx); damage_refuse(src, DMG_REF_SCRATCH); return -1; }   /* no begin, or the lease released it */
    const uint8_t *msg = src + 2;
    uint32_t msglen = srclen - 2u;
    unsigned mode = msg[0] & 0x7fu;
    /* what a step may carry: the shadow messages and the v2 ops that draw or set batch context;
     * a cache write (12, 19) and every non-drawing mode are refused */
    int drawing = mode == 3u || mode == 6u || mode == 8u || mode == 9u ||
                  mode == 13u || mode == 14u || mode == 15u ||
                  (mode >= 17u && mode <= 24u && mode != 19u);
    int rc = -1;
    if (!drawing) damage_refuse(msg, DMG_REF_MODE);
    if (drawing) {
        /* A container state with only what image_dispatch reads: the shadow
         * allocation A (+0x8) and the carrier size (+0x40/+0x42), whose product must
         * cover the packed panel (cfw_shadow_buffer). */
        uint32_t fake_words[0x48 / 4];               /* word-aligned: the pointer store at +0x8 */
        uint8_t *fake = (uint8_t *)fake_words;
        bzero(fake, sizeof(fake_words));
        *(uint8_t **)(fake + 0x8) = scratch;
        *(uint16_t *)(fake + 0x40) = (uint16_t)PANEL_W;
        *(uint16_t *)(fake + 0x42) = (uint16_t)PANEL_H;
        cfw_rectlist rl;
        rl_init(&rl);
        damage_swap_diag(ctx, &ctx->dmg_st_diag);
        rc = image_dispatch(fake, msg, msglen, 1, &rl);   /* mode 23 takes the self-test's slots under the mark */
        damage_swap_diag(ctx, &ctx->dmg_st_diag);
    }
    ctx->dmg_st_seq++;
    ctx->dmg_st_refused = rc == 0 ? 0u : 1u;
    ctx->dmg_st_crc = damage_crc32(scratch, DMG_ST_SHADOW_BYTES);
    damage_self_test_settle(ctx);                /* a release that landed during the step is honoured now */
    return rc;
}
