#include "cfw_context.h"
#include "debug.h"

/*
 * Damage drawing contract v2 — Damage FIRMWARE.md §4 (Phase 2).
 *
 * Context: this repo is the Damage build of the G2 custom firmware for Adam's own
 * glasses (../DAMAGE.md). This file adds image-lane modes 17–24 next to upstream's
 * 3–15, all off until the phone arms flag bit 2 (DRAW2) for the session, and records
 * the reason of every image-lane refusal (v1 modes included) in the telemetry record.
 * Nothing here runs unless a message asks for it; a phone that is gone leaves stock
 * behaviour behind when its lease lapses. Display-rendering work.
 *
 *   17  image draw       [17|80][off4 u16][x s16 (or xL s16 · xR s16)][y s16][options u8]
 *                        a v2 image record at (x, y), clipped to the panel and the batch's clip;
 *                        negative x and y are allowed
 *   18  string draw      [18|80][font off4 u16][x s16 (or xL · xR)][y s16][options u8][len u8][bytes]
 *                        mode 14 over a 224-entry table (codes 32..255, entry code−32, entries
 *                        off4 u16); glyphs are v1 records; bytes 1..31 adjust x by b−11; 0 refused
 *   19  cache write v2   [19]{[off4 u16][len u16][data]}…   mode 12 with 4-byte-unit offsets over
 *                        the session's cache size (op 5); validated whole; bumps the generation
 *   20  clip             [20|80][rect (or rectL · rectR)]   inside a batch only: the v2 draws,
 *                        fills and LUTs after it in the same batch stay inside it
 *   21  fill             [21|80][rect (or pair)][level u8]  nibble-exact fill, no fid, no zlib
 *   22  LUT              [22|80][rect (or pair)][lut 8 B]   every pixel p becomes lut[p]; entry i is
 *                        nibble i of the 8 bytes (high nibble first, as the packed rows)
 *   23  save-under       [23|80][sub u8][slot u8][rect (or pair)] for sub 0 capture;
 *                        [23][1][slot] restore at the captured rect; [23][2][slot] free
 *   24  present hint     [24][y0 u16][y1 u16]  inside a batch only: the batch's present transfers
 *                        rows y0..y1 (inclusive) through the JBD4010's per-row partial entry
 *                        instead of the full refresh (damage_ext.c damage_refresh_hook)
 *
 * The high bit of the mode byte means "lenses differ" exactly as modes 3 and 9: the left
 * lens takes the first rect or x, the right lens the second, one payload. A rect on the
 * wire is [l u16][t u16][w u16][h u16], unquantized, inside the panel. Cache offsets are
 * u16 in 4-byte units (off4). A v2 image record is [w u16][h u16][RLE of w·h pixels], w
 * 1..640, h 1..2048, the v1 token format, no row pad, bounded by the cache's size.
 *
 * Every op validates its whole message before a byte of the shadow, a slot or the cache
 * changes, and a refusal records its reason (damage_refuse). The order of the checks is
 * part of the contract, since the reason is: length (1) → not in a batch (10) → the lease
 * (3) → DRAW2 (9) → the op's own (bounds 2, record 5, code 6, scratch 7, value 12, no
 * memory 11). Damage's simulator (GlassFirmwareSim, Kotlin, written from the contract
 * text) makes the same decisions in the same order; the conformance vectors prove it.
 */

#define DMG_MODE_IMAGE2    17u
#define DMG_MODE_STRING2   18u
#define DMG_MODE_CACHE2    19u
#define DMG_MODE_CLIP      20u
#define DMG_MODE_FILL      21u
#define DMG_MODE_LUT       22u
#define DMG_MODE_SAVE      23u
#define DMG_MODE_HINT      24u

#define DMG_SAVE_BUDGET    (48u * 1024u)   /* the save-under pool, budget A (FIRMWARE.md §4) */
#define DMG_IMAGE2_MAX_W   640u
#define DMG_IMAGE2_MAX_H   2048u
#define DMG_FONT2_CHARS    224u             /* codes 32..255 */
#define DMG_FONT2_BYTES    (DMG_FONT2_CHARS * 2u)

/* ---- the refusal record (fields 23-25) ------------------------------------------------ */

void damage_refuse(const uint8_t *src, unsigned reason) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (ctx == 0) return;
    ctx->dmg_ref_seen = 1;
    ctx->dmg_ref_mode = src ? src[0] : 0xffu;
    ctx->dmg_ref_reason = (uint8_t)reason;
    ctx->dmg_ref_seq = ctx->dmg_present_seq;
}

void damage_clear_refusal(customCfwContext *ctx) {
    if (ctx == 0) return;
    ctx->dmg_ref_seen = 0;
    ctx->dmg_ref_mode = 0;
    ctx->dmg_ref_reason = 0;
    ctx->dmg_ref_seq = 0;
}

/* ---- the cache's size and the flag ------------------------------------------------------ */

uint32_t damage_cache_size(customCfwContext *ctx) {
    if (ctx && ctx->dmg_cache_bytes) return ctx->dmg_cache_bytes;
    return CFW_TEXTURE_CACHE_SIZE;
}

int damage_draw2_armed(customCfwContext *ctx) {
    return ctx != 0 && (ctx->dmg_flags & DMG_FLAG_DRAW2) != 0;
}

/* ---- records in the cache --------------------------------------------------------------- */

/* A v2 image record at byte offset `off`: [w u16][h u16][RLE of w*h pixels], bounded by the
 * session's cache size. Validated whole (every token) before anything draws. */
typedef struct {
    const uint8_t *rle;
    uint32_t rle_len;
    uint32_t width;
    uint32_t height;
} damage_image2;

static int damage_image2_at(customCfwContext *ctx, uint32_t off, damage_image2 *out) {
    if (ctx == 0 || ctx->texture_cache == 0) return 0;
    uint32_t size = damage_cache_size(ctx);
    if (off > size || size - off < 4u) return 0;
    const uint8_t *rec = ctx->texture_cache + off;
    uint32_t width = rd16(rec);
    uint32_t height = rd16(rec + 2);
    if (width == 0 || height == 0 || width > DMG_IMAGE2_MAX_W || height > DMG_IMAGE2_MAX_H) return 0;
    const uint8_t *p = rec + 4;
    uint32_t available = size - off - 4u;
    uint32_t pos = 0;
    uint32_t left = width * height;
    while (left) {
        uint32_t used, count;
        uint8_t color;
        if (!cfw_texture_rle_token(p + pos, available - pos, &used, &count, &color) || count > left)
            return 0;
        pos += used;
        left -= count;
    }
    out->rle = p;
    out->rle_len = pos;
    out->width = width;
    out->height = height;
    return 1;
}

/* A v1 record ([w u8][h u8][RLE]) at byte offset `off`, bounded by the session's cache size
 * rather than the v1 64 KiB window: the v2 string draw's glyphs. */
static int damage_image1_at(customCfwContext *ctx, uint32_t off, cfw_cached_image *out) {
    if (ctx == 0 || ctx->texture_cache == 0) return 0;
    uint32_t size = damage_cache_size(ctx);
    if (off > size || size - off < 2u) return 0;
    const uint8_t *image = ctx->texture_cache + off;
    uint32_t width = image[0];
    uint32_t height = image[1];
    if (width == 0 || height == 0) return 0;
    const uint8_t *p = image + 2;
    uint32_t available = size - off - 2u;
    uint32_t pos = 0;
    uint32_t left = width * height;
    while (left) {
        uint32_t used, count;
        uint8_t color;
        if (!cfw_texture_rle_token(p + pos, available - pos, &used, &count, &color) || count > left)
            return 0;
        pos += used;
        left -= count;
    }
    out->rle = p;
    out->rle_len = pos;
    out->width = width;
    out->height = height;
    return 1;
}

/* ---- the clip and the rendering --------------------------------------------------------- */

/* The area a v2 draw may touch: the panel, cut down by the batch's clip when one is set.
 * Right and bottom are exclusive. */
typedef struct { int32_t l, t, r, b; } damage_clip;

static damage_clip damage_clip_of(const cfw_rectlist *rl, uint32_t panel_w, uint32_t panel_h) {
    damage_clip c;
    c.l = 0; c.t = 0; c.r = (int32_t)panel_w; c.b = (int32_t)panel_h;
    if (rl && rl->clip_on) {
        if ((int32_t)rl->clip_l > c.l) c.l = rl->clip_l;
        if ((int32_t)rl->clip_t > c.t) c.t = rl->clip_t;
        if ((int32_t)rl->clip_r < c.r) c.r = rl->clip_r;
        if ((int32_t)rl->clip_b < c.b) c.b = rl->clip_b;
    }
    return c;
}

static void damage_put_nibble(uint8_t *shadow, uint32_t stride, int32_t x, int32_t y, uint8_t v) {
    uint8_t *b = shadow + (uint32_t)y * stride + ((uint32_t)x >> 1);
    if (x & 1) *b = (uint8_t)((*b & 0xf0u) | v);
    else       *b = (uint8_t)((*b & 0x0fu) | (uint8_t)(v << 4));
}

static uint8_t damage_get_nibble(const uint8_t *shadow, uint32_t stride, int32_t x, int32_t y) {
    uint8_t b = shadow[(uint32_t)y * stride + ((uint32_t)x >> 1)];
    return (x & 1) ? (uint8_t)(b & 0x0fu) : (uint8_t)(b >> 4);
}

/* Render a validated run stream of `width` x `height` pixels at (x0, y0), clipped to `c`.
 * Transparency tests the source level before the LUT, as the v1 renderer does. Runs that
 * fall outside the clip are skipped a row at a time. */
static void damage_render_runs(uint8_t *shadow, uint32_t stride, const damage_clip *c,
                               int32_t x0, int32_t y0, const uint8_t *rle, uint32_t rle_len,
                               uint32_t width, uint32_t height, const uint8_t *lut, int transparent) {
    (void)height;
    uint32_t pos = 0;
    int32_t x = 0, y = 0;                    /* the cursor inside the image */
    while (pos < rle_len) {
        uint32_t used, count;
        uint8_t color;
        if (!cfw_texture_rle_token(rle + pos, rle_len - pos, &used, &count, &color)) return;   /* validated: never */
        pos += used;
        int skip = transparent && color == 0;
        uint8_t mapped = lut[color];
        while (count) {
            int32_t py = y0 + y;
            uint32_t room = width - (uint32_t)x;         /* pixels left in this image row */
            uint32_t take = count < room ? count : room;
            if (!skip && py >= c->t && py < c->b) {
                int32_t px = x0 + x;
                for (uint32_t i = 0; i < take; i++, px++)
                    if (px >= c->l && px < c->r) damage_put_nibble(shadow, stride, px, py, mapped);
            }
            count -= take;
            x += (int32_t)take;
            if ((uint32_t)x >= width) { x = 0; y++; }
        }
    }
}

/* The rect list entry a v2 draw contributes: its box cut to the clip (empty = nothing). */
static void damage_add_rect(cfw_rectlist *rl, const damage_clip *c, int32_t x, int32_t y,
                            uint32_t w, uint32_t h) {
    int32_t l = x < c->l ? c->l : x;
    int32_t t = y < c->t ? c->t : y;
    int32_t r = x + (int32_t)w; if (r > c->r) r = c->r;
    int32_t b = y + (int32_t)h; if (b > c->b) b = c->b;
    if (l < r && t < b) rl_add(rl, (uint32_t)l, (uint32_t)t, (uint32_t)(r - l), (uint32_t)(b - t));
}

/* Read a wire rect; 1 when it is inside the panel and not empty. */
static int damage_read_rect(const uint8_t *p, uint32_t panel_w, uint32_t panel_h,
                            uint32_t *l, uint32_t *t, uint32_t *w, uint32_t *h) {
    *l = rd16(p); *t = rd16(p + 2); *w = rd16(p + 4); *h = rd16(p + 6);
    if (*w == 0 || *h == 0 || *l + *w > panel_w || *t + *h > panel_h) return 0;
    return 1;
}

/* ---- save-under slots -------------------------------------------------------------------- */

static void damage_slot_free(damage_slot *s, uint32_t *bytes) {
    uint8_t *buf = s->buf;
    if (buf == 0) return;
    s->buf = 0;
    if (*bytes >= s->bytes) *bytes -= s->bytes; else *bytes = 0;
    s->bytes = 0;
    cfw_heap13_free(buf);
}

void damage_slots_free(damage_slot *slots, uint32_t *bytes) {
    for (uint32_t i = 0; i < DMG_SLOTS; i++) damage_slot_free(&slots[i], bytes);
    *bytes = 0;
}

void damage_slots_swap(customCfwContext *ctx) {
    for (uint32_t i = 0; i < DMG_SLOTS; i++) {
        damage_slot t = ctx->dmg_slots[i];
        ctx->dmg_slots[i] = ctx->dmg_st_slots[i];
        ctx->dmg_st_slots[i] = t;
    }
    uint32_t b = ctx->dmg_slot_bytes;
    ctx->dmg_slot_bytes = ctx->dmg_st_slot_bytes;
    ctx->dmg_st_slot_bytes = b;
}

/* ---- the ops ------------------------------------------------------------------------------ */

/* Mode 17. Payload after the mode byte: 7 (one x) or 9 (two x). */
static int damage_op_image(customCfwContext *ctx, uint8_t *shadow, uint32_t stride,
                           uint32_t panel_w, uint32_t panel_h, const uint8_t *src, uint32_t srclen,
                           cfw_rectlist *rl, int lenses_differ) {
    uint32_t need = lenses_differ ? 10u : 8u;
    if (srclen != need) { damage_refuse(src, DMG_REF_LENGTH); return -1; }
    if (!cfw_fb_lease_active()) { damage_refuse(src, DMG_REF_NO_LEASE); return -1; }
    if (!damage_draw2_armed(ctx)) { damage_refuse(src, DMG_REF_DRAW2); return -1; }
    uint32_t off = rd16(src + 1) * 4u;
    const uint8_t *p = src + 3;
    int32_t x;
    if (lenses_differ) {
        x = (int32_t)(int16_t)rd16(FW_SIDE() == 2 ? p : p + 2);
        p += 4;
    } else {
        x = (int32_t)(int16_t)rd16(p);
        p += 2;
    }
    int32_t y = (int32_t)(int16_t)rd16(p);
    uint8_t options = p[2];
    damage_image2 img;
    if (!damage_image2_at(ctx, off, &img)) { damage_refuse(src, DMG_REF_RECORD); return -1; }
    uint8_t lut[16];
    cfw_texture_make_lut(options, lut);
    damage_clip c = damage_clip_of(rl, panel_w, panel_h);
    damage_render_runs(shadow, stride, &c, x, y, img.rle, img.rle_len, img.width, img.height,
                       lut, (options & CFW_TEXTURE_OPT_TRANSPARENT) != 0);
    damage_add_rect(rl, &c, x, y, img.width, img.height);
    return 0;
}

/* Mode 18. Payload after the mode byte: 8 + len (one x) or 10 + len (two x). */
static int damage_op_string(customCfwContext *ctx, uint8_t *shadow, uint32_t stride,
                            uint32_t panel_w, uint32_t panel_h, const uint8_t *src, uint32_t srclen,
                            cfw_rectlist *rl, int lenses_differ) {
    uint32_t head = lenses_differ ? 11u : 9u;              /* mode byte + header */
    if (srclen < head) { damage_refuse(src, DMG_REF_LENGTH); return -1; }
    uint32_t string_len = src[head - 1];
    if (srclen != head + string_len) { damage_refuse(src, DMG_REF_LENGTH); return -1; }
    if (!cfw_fb_lease_active()) { damage_refuse(src, DMG_REF_NO_LEASE); return -1; }
    if (!damage_draw2_armed(ctx)) { damage_refuse(src, DMG_REF_DRAW2); return -1; }
    uint32_t font = rd16(src + 1) * 4u;
    const uint8_t *p = src + 3;
    int32_t x;
    if (lenses_differ) {
        x = (int32_t)(int16_t)rd16(FW_SIDE() == 2 ? p : p + 2);
        p += 4;
    } else {
        x = (int32_t)(int16_t)rd16(p);
        p += 2;
    }
    int32_t y = (int32_t)(int16_t)rd16(p);
    uint8_t options = p[2];
    const uint8_t *string = src + head;
    uint32_t size = damage_cache_size(ctx);
    if (ctx->texture_cache == 0 || font > size || size - font < DMG_FONT2_BYTES) {
        damage_refuse(src, DMG_REF_RECORD); return -1;
    }
    const uint8_t *table = ctx->texture_cache + font;
    /* validate every byte, table entry and glyph record before a pixel */
    for (uint32_t i = 0; i < string_len; i++) {
        uint32_t ch = string[i];
        if (ch >= 1u && ch <= 31u) continue;
        if (ch < 32u) { damage_refuse(src, DMG_REF_CODE); return -1; }
        cfw_cached_image g;
        if (!damage_image1_at(ctx, rd16(table + (ch - 32u) * 2u) * 4u, &g)) {
            damage_refuse(src, DMG_REF_RECORD); return -1;
        }
    }
    uint8_t lut[16];
    cfw_texture_make_lut(options, lut);
    int transparent = (options & CFW_TEXTURE_OPT_TRANSPARENT) != 0;
    damage_clip c = damage_clip_of(rl, panel_w, panel_h);
    for (uint32_t i = 0; i < string_len; i++) {
        uint32_t ch = string[i];
        if (ch <= 31u) { x += (int32_t)ch - 11; continue; }
        cfw_cached_image g;
        if (!damage_image1_at(ctx, rd16(table + (ch - 32u) * 2u) * 4u, &g)) return -1;   /* validated above */
        damage_render_runs(shadow, stride, &c, x, y, g.rle, g.rle_len, g.width, g.height, lut, transparent);
        damage_add_rect(rl, &c, x, y, g.width, g.height);
        x += (int32_t)g.width;
    }
    return 0;
}

/* Mode 19: [19]{[off4 u16][len u16][data]}… over the session's cache size. Like mode 12 the
 * whole list is checked first and an all-empty list is a no-op; the cache is allocated at
 * the session's size on the first write that carries data. */
static int damage_op_cache_write(customCfwContext *ctx, const uint8_t *src, uint32_t srclen) {
    uint32_t size = damage_cache_size(ctx);
    uint32_t pos = 1;
    int has_data = 0;
    while (pos < srclen) {
        if (srclen - pos < 4u) { damage_refuse(src, DMG_REF_LENGTH); return -1; }
        uint32_t off = rd16(src + pos) * 4u;
        uint32_t len = rd16(src + pos + 2u);
        pos += 4u;
        if (len > srclen - pos) { damage_refuse(src, DMG_REF_LENGTH); return -1; }
        if (off > size || len > size - off) { damage_refuse(src, DMG_REF_RECORD); return -1; }
        if (len) has_data = 1;
        pos += len;
    }
    if (!has_data) return 0;
    if (!cfw_fb_lease_active()) { damage_refuse(src, DMG_REF_NO_LEASE); return -1; }
    if (!damage_draw2_armed(ctx)) { damage_refuse(src, DMG_REF_DRAW2); return -1; }
    if (ctx->texture_cache == 0) {
        uint8_t *cache = (uint8_t *)cfw_heap13_malloc(size);
        if (cache == 0) { damage_refuse(src, DMG_REF_NO_MEMORY); return -1; }
        bzero(cache, size);
        ctx->texture_cache = cache;
        ctx->dmg_cache_bytes = size;             /* the allocated size, reported as field 18 */
    }
    pos = 1;
    while (pos < srclen) {
        uint32_t off = rd16(src + pos) * 4u;
        uint32_t len = rd16(src + pos + 2u);
        pos += 4u;
        for (uint32_t i = 0; i < len; i++) ctx->texture_cache[off + i] = src[pos + i];
        pos += len;
    }
    ctx->dmg_cache_gen++;
    return 0;
}

/* Mode 20: [20|80][rect (or rectL · rectR)], inside a batch only. */
static int damage_op_clip(customCfwContext *ctx, uint32_t panel_w, uint32_t panel_h,
                          const uint8_t *src, uint32_t srclen, int present, cfw_rectlist *rl,
                          int lenses_differ) {
    uint32_t need = lenses_differ ? 17u : 9u;
    if (srclen != need) { damage_refuse(src, DMG_REF_LENGTH); return -1; }
    if (present) { damage_refuse(src, DMG_REF_NO_BATCH); return -1; }
    if (!cfw_fb_lease_active()) { damage_refuse(src, DMG_REF_NO_LEASE); return -1; }
    if (!damage_draw2_armed(ctx)) { damage_refuse(src, DMG_REF_DRAW2); return -1; }
    const uint8_t *p = src + 1;
    if (lenses_differ && FW_SIDE() != 2) p += 8;
    uint32_t l, t, w, h;
    if (!damage_read_rect(p, panel_w, panel_h, &l, &t, &w, &h)) { damage_refuse(src, DMG_REF_BOUNDS); return -1; }
    rl->clip_on = 1;
    rl->clip_l = (uint16_t)l; rl->clip_t = (uint16_t)t;
    rl->clip_r = (uint16_t)(l + w); rl->clip_b = (uint16_t)(t + h);
    return 0;
}

/* Mode 21: [21|80][rect (or pair)][level u8]. */
static int damage_op_fill(customCfwContext *ctx, uint8_t *shadow, uint32_t stride,
                          uint32_t panel_w, uint32_t panel_h, const uint8_t *src, uint32_t srclen,
                          cfw_rectlist *rl, int lenses_differ) {
    uint32_t need = lenses_differ ? 18u : 10u;
    if (srclen != need) { damage_refuse(src, DMG_REF_LENGTH); return -1; }
    if (!cfw_fb_lease_active()) { damage_refuse(src, DMG_REF_NO_LEASE); return -1; }
    if (!damage_draw2_armed(ctx)) { damage_refuse(src, DMG_REF_DRAW2); return -1; }
    const uint8_t *p = src + 1;
    if (lenses_differ && FW_SIDE() != 2) p += 8;
    uint32_t l, t, w, h;
    if (!damage_read_rect(p, panel_w, panel_h, &l, &t, &w, &h)) { damage_refuse(src, DMG_REF_BOUNDS); return -1; }
    uint32_t level = src[need - 1];
    if (level > 15u) { damage_refuse(src, DMG_REF_VALUE); return -1; }
    damage_clip c = damage_clip_of(rl, panel_w, panel_h);
    int32_t x0 = (int32_t)l < c.l ? c.l : (int32_t)l;
    int32_t y0 = (int32_t)t < c.t ? c.t : (int32_t)t;
    int32_t x1 = (int32_t)(l + w) > c.r ? c.r : (int32_t)(l + w);
    int32_t y1 = (int32_t)(t + h) > c.b ? c.b : (int32_t)(t + h);
    uint8_t pair = (uint8_t)(level * 0x11u);
    for (int32_t y = y0; y < y1; y++) {
        int32_t x = x0;
        if (x < x1 && (x & 1)) { damage_put_nibble(shadow, stride, x, y, (uint8_t)level); x++; }
        uint8_t *row = shadow + (uint32_t)y * stride;
        while (x + 1 < x1) { row[(uint32_t)x >> 1] = pair; x += 2; }
        if (x < x1) damage_put_nibble(shadow, stride, x, y, (uint8_t)level);
    }
    damage_add_rect(rl, &c, (int32_t)l, (int32_t)t, w, h);
    return 0;
}

/* Mode 22: [22|80][rect (or pair)][lut 8 B]; entry i is nibble i, high nibble first. */
static int damage_op_lut(customCfwContext *ctx, uint8_t *shadow, uint32_t stride,
                         uint32_t panel_w, uint32_t panel_h, const uint8_t *src, uint32_t srclen,
                         cfw_rectlist *rl, int lenses_differ) {
    uint32_t need = lenses_differ ? 25u : 17u;
    if (srclen != need) { damage_refuse(src, DMG_REF_LENGTH); return -1; }
    if (!cfw_fb_lease_active()) { damage_refuse(src, DMG_REF_NO_LEASE); return -1; }
    if (!damage_draw2_armed(ctx)) { damage_refuse(src, DMG_REF_DRAW2); return -1; }
    const uint8_t *p = src + 1;
    if (lenses_differ && FW_SIDE() != 2) p += 8;
    uint32_t l, t, w, h;
    if (!damage_read_rect(p, panel_w, panel_h, &l, &t, &w, &h)) { damage_refuse(src, DMG_REF_BOUNDS); return -1; }
    const uint8_t *lb = src + need - 8u;
    uint8_t lut[16];
    for (uint32_t i = 0; i < 16u; i++) lut[i] = (i & 1u) ? (uint8_t)(lb[i >> 1] & 0x0fu) : (uint8_t)(lb[i >> 1] >> 4);
    damage_clip c = damage_clip_of(rl, panel_w, panel_h);
    int32_t x0 = (int32_t)l < c.l ? c.l : (int32_t)l;
    int32_t y0 = (int32_t)t < c.t ? c.t : (int32_t)t;
    int32_t x1 = (int32_t)(l + w) > c.r ? c.r : (int32_t)(l + w);
    int32_t y1 = (int32_t)(t + h) > c.b ? c.b : (int32_t)(t + h);
    for (int32_t y = y0; y < y1; y++)
        for (int32_t x = x0; x < x1; x++)
            damage_put_nibble(shadow, stride, x, y, lut[damage_get_nibble(shadow, stride, x, y)]);
    damage_add_rect(rl, &c, (int32_t)l, (int32_t)t, w, h);
    return 0;
}

/* Mode 23: sub 0 [23|80][0][slot][rect (or pair)] capture · sub 1 [23][1][slot] restore at the
 * captured rect · sub 2 [23][2][slot] free (an empty slot is freed silently). Capture into a
 * used slot replaces it. The slots are per lens by construction (each lens runs the op).
 * Returns 1 when the shadow changed (a restore), 0 for a capture or a free, -1 refused. */
static int damage_op_save(customCfwContext *ctx, uint8_t *shadow, uint32_t stride,
                          uint32_t panel_w, uint32_t panel_h, const uint8_t *src, uint32_t srclen,
                          cfw_rectlist *rl, int lenses_differ) {
    if (srclen < 3u) { damage_refuse(src, DMG_REF_LENGTH); return -1; }
    uint32_t sub = src[1];
    uint32_t slot = src[2];
    uint32_t need = sub == 0u ? (lenses_differ ? 19u : 11u) : 3u;
    if (srclen != need) { damage_refuse(src, DMG_REF_LENGTH); return -1; }
    if (!cfw_fb_lease_active()) { damage_refuse(src, DMG_REF_NO_LEASE); return -1; }
    if (!damage_draw2_armed(ctx)) { damage_refuse(src, DMG_REF_DRAW2); return -1; }
    if (sub > 2u) { damage_refuse(src, DMG_REF_VALUE); return -1; }
    if (slot >= DMG_SLOTS) { damage_refuse(src, DMG_REF_SCRATCH); return -1; }
    damage_slot *s = &ctx->dmg_slots[slot];
    if (sub == 2u) { damage_slot_free(s, &ctx->dmg_slot_bytes); return 0; }
    if (sub == 1u) {
        if (s->buf == 0) { damage_refuse(src, DMG_REF_SCRATCH); return -1; }
        uint32_t rowbytes = ((uint32_t)s->w + 1u) >> 1;
        for (uint32_t y = 0; y < s->h; y++) {
            const uint8_t *row = s->buf + y * rowbytes;
            for (uint32_t x = 0; x < s->w; x++) {
                uint8_t v = (x & 1u) ? (uint8_t)(row[x >> 1] & 0x0fu) : (uint8_t)(row[x >> 1] >> 4);
                damage_put_nibble(shadow, stride, (int32_t)(s->l + x), (int32_t)(s->t + y), v);
            }
        }
        rl_add(rl, s->l, s->t, s->w, s->h);
        return 1;
    }
    const uint8_t *p = src + 3;
    if (lenses_differ && FW_SIDE() != 2) p += 8;
    uint32_t l, t, w, h;
    if (!damage_read_rect(p, panel_w, panel_h, &l, &t, &w, &h)) { damage_refuse(src, DMG_REF_BOUNDS); return -1; }
    uint32_t rowbytes = (w + 1u) >> 1;
    uint32_t bytes = rowbytes * h;
    uint32_t held = ctx->dmg_slot_bytes - (s->buf ? s->bytes : 0u);   /* the slot's own bytes are replaced */
    if (held + bytes > DMG_SAVE_BUDGET) { damage_refuse(src, DMG_REF_SCRATCH); return -1; }
    uint8_t *buf = (uint8_t *)cfw_heap13_malloc(bytes);
    if (buf == 0) { damage_refuse(src, DMG_REF_NO_MEMORY); return -1; }
    damage_slot_free(s, &ctx->dmg_slot_bytes);
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = buf + y * rowbytes;
        for (uint32_t x = 0; x < rowbytes; x++) row[x] = 0;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t v = damage_get_nibble(shadow, stride, (int32_t)(l + x), (int32_t)(t + y));
            if (x & 1u) row[x >> 1] = (uint8_t)(row[x >> 1] | v);
            else        row[x >> 1] = (uint8_t)(v << 4);
        }
    }
    s->buf = buf;
    s->l = (uint16_t)l; s->t = (uint16_t)t; s->w = (uint16_t)w; s->h = (uint16_t)h;
    s->bytes = bytes;
    ctx->dmg_slot_bytes += bytes;
    return 0;
}

/* Mode 24: [24][y0 u16][y1 u16], inside a batch only; rows inclusive, y0 <= y1 <= 479. */
static int damage_op_hint(customCfwContext *ctx, uint32_t panel_h, const uint8_t *src, uint32_t srclen,
                          int present, cfw_rectlist *rl) {
    if (srclen != 5u) { damage_refuse(src, DMG_REF_LENGTH); return -1; }
    if (present) { damage_refuse(src, DMG_REF_NO_BATCH); return -1; }
    if (!cfw_fb_lease_active()) { damage_refuse(src, DMG_REF_NO_LEASE); return -1; }
    if (!damage_draw2_armed(ctx)) { damage_refuse(src, DMG_REF_DRAW2); return -1; }
    uint32_t y0 = rd16(src + 1), y1 = rd16(src + 3);
    if (y0 > y1 || y1 >= panel_h) { damage_refuse(src, DMG_REF_BOUNDS); return -1; }
    rl->hint_on = 1;
    rl->hint_y0 = (uint16_t)y0;
    rl->hint_y1 = (uint16_t)y1;
    return 0;
}

/* ---- the dispatcher's door ------------------------------------------------------------------ */

/* Modes 17–24 from image_dispatch (zlib_glue.c): `present` is 1 at the top level (present the
 * shadow after a mutation), 0 inside a batch. Returns the firmware's 0 / -1. */
int damage_dispatch_v2(uint8_t *state, const uint8_t *src, uint32_t srclen, int present, void *rl_) {
    cfw_rectlist *rl = (cfw_rectlist *)rl_;
    customCfwContext *ctx = getCustomCfwContext();
    if (ctx == 0 || src == 0 || srclen < 1u) return -1;
    int lenses_differ = (src[0] & 0x80u) != 0;
    uint32_t mode = src[0] & 0x7fu;
    uint32_t w = PANEL_W, h = PANEL_H;
    if (mode == DMG_MODE_CACHE2) return damage_op_cache_write(ctx, src, srclen);
    if (mode == DMG_MODE_CLIP) return damage_op_clip(ctx, w, h, src, srclen, present, rl, lenses_differ);
    if (mode == DMG_MODE_HINT) return damage_op_hint(ctx, h, src, srclen, present, rl);
    uint8_t *shadow = cfw_shadow_buffer(state);
    if (shadow == 0) { damage_refuse(src, DMG_REF_NO_SHADOW); return -1; }
    uint32_t stride = (w + 1u) >> 1;
    int r;
    switch (mode) {
    case DMG_MODE_IMAGE2:  r = damage_op_image(ctx, shadow, stride, w, h, src, srclen, rl, lenses_differ); break;
    case DMG_MODE_STRING2: r = damage_op_string(ctx, shadow, stride, w, h, src, srclen, rl, lenses_differ); break;
    case DMG_MODE_FILL:    r = damage_op_fill(ctx, shadow, stride, w, h, src, srclen, rl, lenses_differ); break;
    case DMG_MODE_LUT:     r = damage_op_lut(ctx, shadow, stride, w, h, src, srclen, rl, lenses_differ); break;
    case DMG_MODE_SAVE: {
        r = damage_op_save(ctx, shadow, stride, w, h, src, srclen, rl, lenses_differ);
        if (r < 0) return -1;
        if (r == 1 && present) present_shadow(state, w, h, rl);   /* a restore changed the shadow */
        return 0;
    }
    default: damage_refuse(src, DMG_REF_MODE); return -1;
    }
    if (r != 0) return -1;
    if (present) present_shadow(state, w, h, rl);
    return 0;
}
