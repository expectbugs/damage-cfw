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
