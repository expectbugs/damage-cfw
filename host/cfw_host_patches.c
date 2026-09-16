/*
 * The patch sources as one translation unit, for the x86 host build (host/README.md).
 *
 * Built with -m32 -DCFW_HOST. The firmware addresses the sources call and read are
 * backed by fixed mappings that cfw_host_shim.c sets up before any patch function
 * runs; nothing here runs on, or is sent to, the glasses. The two entry shims
 * written in ARM assembly are left out by their #ifndef CFW_HOST guards.
 *
 * Below the include: small non-static doors into the unit's static functions, so
 * the shim (a separate unit that needs libc headers) can drive the same entry
 * points the firmware uses.
 */
#include "../patches/patches_main.c"

int host_image_worker(void *state, uint8_t *src, uint32_t len) {
    return image_worker(state, src, len);
}

/* The body of sid-0x09 field 101 (['F','C',1,op,nonceLo,nonceHi]), exactly what
 * settings_decode_wrapper hands to the control parser. */
void host_settings_control(const uint8_t *data, uint32_t len) {
    faceclaw_apply_control(data, len);
}

/* Create the context early and give the timing calibration a value, so no
 * measurement spins waiting for a tick the host does not advance. */
void host_context_prime(void) {
    customCfwContext *ctx = getCustomCfwContext();
    if (ctx) ctx->cyc_per_ms = 250000u;
}

/* The sticky diagnostics: out[0..4] = reorder, skip, dup, snapshot overflow,
 * allocation failure. Returns 0 when there is no context yet. */
int host_flags(uint8_t *out) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx) return 0;
    out[0] = ctx->f_reorder; out[1] = ctx->f_skip; out[2] = ctx->f_dup;
    out[3] = ctx->f_snap_of; out[4] = (uint8_t)(cfw_alloc_diag() & 1u);
    return 1;
}

/* The Damage extension's state (damage_ext.c), for tests of what the left lens holds
 * but cannot report: flags, the status register, the cache generation and the
 * cache-keep latch, the direct presents and the last transfer stamp, the self-test's
 * step count, last refusal and scratch CRC, whether a texture cache is allocated, and
 * the direct-frame flag (zlib_glue.c direct_active: the physical framebuffer holds a
 * Damage frame that stock repaints must not overwrite while the lease holds). */
void host_damage_state(uint32_t *out) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx) return;
    out[0] = ctx->dmg_flags; out[1] = ctx->dmg_status; out[2] = ctx->dmg_cache_gen;
    out[3] = ctx->dmg_cache_keep_latched; out[4] = ctx->dmg_present_seq; out[5] = ctx->dmg_last_transfer_us;
    out[6] = ctx->dmg_st_seq; out[7] = ctx->dmg_st_refused; out[8] = ctx->dmg_st_crc;
    out[9] = ctx->texture_cache != 0;
    out[10] = ctx->direct_active;
    /* Phase 2 (damage_draw.c): the refusal record, the cache's size, the last transfer's path,
     * (out[17] is the shim's partial count) and the save-under slots' bytes */
    out[11] = ctx->dmg_ref_seen; out[12] = ctx->dmg_ref_mode; out[13] = ctx->dmg_ref_reason; out[14] = ctx->dmg_ref_seq;
    out[15] = ctx->texture_cache ? damage_cache_size(ctx) : 0u;
    out[16] = ctx->dmg_last_path;
    out[18] = ctx->dmg_slot_bytes;
    out[19] = ctx->dmg_direct_presented;   /* the F1.3 mark: whose frame the next refresh stamps */
}
