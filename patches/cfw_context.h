#pragma once
#include <stdint.h>

/* Persistent CFW-owned state that must survive image-container teardown/rebuild.
 * The image container (its display buffer A @ state+0x8 and recon buffer B @
 * state+0xc) is freed and reallocated on rebuild. The packed shadow lives in A
 * only for the lifetime of the current streaming layout and must be seeded by a
 * mode-6 keyframe after every rebuild. The bookkeeping that does
 * need to survive rebuilds is anchored by a pointer in 1 KiB of SRAM explicitly
 * removed from the top of the stock primary TLSF arena by patch_compress.py. The
 * stock arena is [0x20279670,0x202a6670); the patched size is 0x2cc00, reserving
 * [0x202a6270,0x202a6670) for CFW. Its first word holds the context pointer and
 * its second holds a magic-guarded sticky allocation-failure diagnostic. This is
 * deliberately carved out rather than
 * inferred padding: 0x20003ffc, used before EVENCFW/11, is actually the +0 callback
 * of the BLE-RX lifecycle object and stock code can BLX through it. The struct's
 * `magic` guards against warm-reset garbage; the slot ptr is range-checked before
 * dereference. */

#define CFW_FID_RING  16     /* recent mode-3 frame ids kept for duplicate detection */
#define CFW_SNAP_RING 12     /* in-flight compressed-message snapshots (per producer race depth) */
#define CFW_SNAP_BUSY_SEQ 0xffffffffU /* range is reserved by the deferred worker */
#define CFW_SEQ_MAX   48     /* max steps in a buzzer tone sequence (mode-5 kind 4) */

/* The mode-3 frame-order diagnostics as one block, so the Damage self-test
 * (damage_ext.c) can swap its own in for a step and restore the live one after. */
typedef struct {
    uint16_t last_fid;
    uint16_t high_fid;
    uint8_t  diag_seen;
    uint8_t  fid_resync;
    uint8_t  f_reorder;
    uint8_t  f_skip;
    uint8_t  f_dup;
    uint8_t  recent_pos;
    uint16_t recent_fids[CFW_FID_RING];
} damage_diag_state;

/* One snapshotted compressed image message. Taken at reconstruction-complete (both
 * lenses), consumed FIFO in the deferred handler. Keyed by the owning image-state
 * pointer so multiple containers (e.g. faceclaw's 4 tiles) don't cross-feed. */
typedef struct {
    uint8_t *state;      /* owning image-state (key); 0 = empty slot */
    uint8_t *buf;        /* copy packed into this state's reconstruction-buffer tail */
    uint32_t len;
    volatile uint32_t seq; /* push order, or CFW_SNAP_BUSY_SEQ while being consumed */
} cfw_snap;

/* One save-under slot (Damage FIRMWARE.md §4, mode 23): the captured rect and its pixels
 * as tight packed rows ((w+1)/2 bytes per row, high nibble = left pixel), from heap 13. */
#define DMG_SLOTS 4
typedef struct {
    uint8_t *buf;        /* 0 = empty */
    uint16_t l, t, w, h;
    uint32_t bytes;
} damage_slot;

/* Reasons of an image-lane refusal (telemetry field 24; Damage FIRMWARE.md §4). */
#define DMG_REF_LENGTH     1u   /* a message or entry shorter or longer than its shape */
#define DMG_REF_BOUNDS     2u   /* a rect, box, clip, row range or level outside its range */
#define DMG_REF_NO_LEASE   3u
#define DMG_REF_NO_SHADOW  4u   /* no shadow buffer (the container's display allocation) */
#define DMG_REF_RECORD     5u   /* a cache offset, record or table outside the cache, or a malformed record */
#define DMG_REF_CODE       6u   /* a string byte that is neither an adjust nor a glyph */
#define DMG_REF_SCRATCH    7u   /* a save-under slot: out of range, empty on restore, over budget; or the self-test's scratch */
#define DMG_REF_MODE       8u   /* a mode this build has no handler for, or one not allowed where it sits */
#define DMG_REF_DRAW2      9u   /* a v2 op while flag bit 2 is not armed */
#define DMG_REF_NO_BATCH  10u   /* a clip or a present hint outside a batch */
#define DMG_REF_NO_MEMORY 11u   /* an allocation from heap 13 failed */
#define DMG_REF_VALUE     12u   /* a sub-op, a LUT, a level or another value outside its range */
#define DMG_REF_STREAM    13u   /* a zlib or RLE stream that does not decode to its size */

typedef struct {
    uint32_t magic;      /* CFW_CTX_MAGIC when valid */
    /* --- snapshot FIFO: fixes the producer/consumer race on the shared recon buffer.
     * snapshot_side() copies each completed message here (both lenses); image_deferred
     * drains this lens's pending snapshots and runs the worker on each, ignoring the
     * live (possibly-overwritten) recon buffer B. (The 4bpp shadow of the last frame,
     * needed by mode-3 deltas, reuses each container's display buffer A — see
     * cfw_shadow_buffer — so it's per-container and costs no extra RAM.) */
    cfw_snap snaps[CFW_SNAP_RING];
    uint32_t snap_seq;   /* next push sequence number */
    /* --- diagnostics, overlaid as a text line (verify the fix; should stay clear). Mode
     * 7 clears the flags / toggles the overlay visibility (diag_hide). --- */
    uint16_t last_fid;   /* last frame id seen (mode-3 messages) */
    uint16_t high_fid;   /* highest frame id seen */
    uint8_t  diag_seen;  /* recorded at least one frame yet */
    uint8_t  fid_resync; /* keyframe rebaselines the next delta's fid (no false skip) */
    uint8_t  diag_hide;  /* 1 = don't draw the flag overlay (default 0 = visible) */
    uint32_t last_worker_us;  /* image_worker() duration of the PREVIOUS message (overlay) */
    uint32_t last_present_us; /* present_shadow() duration of the PREVIOUS present (overlay) */
    uint32_t cyc_per_ms;      /* calibrated DWT cycles per 1 ms OS tick (0 = not yet done) */
    uint8_t  f_reorder;  /* FLAG: ever saw a frame id go backward */
    uint8_t  f_skip;     /* FLAG: ever saw a frame id gap (skipped) */
    uint8_t  f_dup;      /* FLAG: ever saw a duplicate frame id (in the recent ring) */
    uint8_t  f_snap_of;  /* FLAG: snapshot ring overflowed (dropped an in-flight frame) */
    uint16_t recent_fids[CFW_FID_RING]; /* ring of the last N mode-3 frame ids seen */
    uint8_t  recent_pos; /* next write index into recent_fids */
    /* --- buzzer tone sequencer (mode-5 kind 4). Plays a list of (freq,duty,ms)
     * steps back-to-back on OUR OWN one-shot osTimer — the firmware buzzer timer's
     * callback is the fixed note-walker, which can't emit arbitrary frequencies.
     * State lives in this singleton so it survives the handler return and is
     * reachable from seq_tick (the timer callback, in the RTOS timer thread). --- */
    uint32_t seq_timer;                   /* our osTimer handle; created lazily, reused, never freed */
    uint8_t  seq_count;                   /* steps in the current sequence (0 = idle) */
    uint8_t  seq_cursor;                  /* index of the next step to play */
    uint8_t  seq_steps[CFW_SEQ_MAX * 5];  /* freqLo,freqHi,duty,msLo,msHi per step */
    /* --- Faceclaw wake takeover. A volatile, fail-open ownership lease lets
     * Faceclaw defer the stock dashboard only while its phone process is
     * demonstrably alive. See settings_ext.c for the private sid-0x09 control
     * protocol and the double-tap / Even AI entry hooks. */
    uint32_t wake_lease_deadline;          /* FW_MS_TICK deadline; 0 = no owner */
    uint32_t wake_fallback_timer;          /* one-shot stock-dashboard fallback */
    uint16_t wake_nonce;                   /* current pending wake, 0 = none */
    uint8_t  wake_dashboard_pending;       /* dashboard request held for Faceclaw */
    volatile uint8_t compass_forward;      /* mode 10: forward global heading events to BLE */
    uint8_t  wake_notify_buf[16];          /* stable storage for sid-0x09 notify */
    uint8_t  wear_notify_buf[12];          /* stable storage for sid-0x10 wear notify */
    /* Direct-framebuffer job. The EvenHub worker holds the stock display gate
     * before it mutates the shadow and until the display task consumes this
     * pointer, so no second snapshot or full-size display buffer is required. */
    const uint8_t *direct_shadow;
    volatile uint8_t direct_pending;
    uint8_t direct_failed;
    uint8_t direct_active;                    /* physical framebuffer currently owns the image */
    uint32_t direct_lease_deadline;            /* fail-open repaint-guard deadline */
    /* Phone-owned texture data, allocated lazily on the first mode-12 write and
     * released with the Faceclaw framebuffer lease. Protocol references into
     * this block are uint16 offsets. */
    uint8_t *texture_cache;
    /* --- Microphone control + multi-channel routing (SybilSight "glasses ->
     * microphones"). See the contract comment in mic_control.c; the stock-entry
     * recovery evidence lives in evenRealities-openCFW/g2/docs/research/
     * (g2-service-audio-recovery.md, g2-service-algo-recovery.md,
     * g2-production-mic-recovery.md). Config is advertised/read back over
     * sid-0x09 fields 103/104; capture + streaming are gated behind
     * MIC_FLAG_ARM_HW plus a fail-open renewal lease. Appended at the tail so
     * every existing field offset is unchanged. --- */
    uint8_t  mic_active;                    /* 1 = a CFW mic configuration is in effect */
    uint8_t  mic_source;                    /* 0 = codec DMIC/I2S, 1 = Ambiq PDM mics */
    uint8_t  mic_channels;                  /* requested channel count (1 = mono, 2 = dual) */
    uint8_t  mic_chan_mask;                 /* per-mic enable bitmask (bit0=front, bit1=rear) */
    uint8_t  mic_codec;                     /* requested: 0 = LC3 encoded, 1 = raw PCM passthrough */
    uint8_t  mic_format;                    /* PCM width: 0=16-bit, 1=24-bit, 2=32-bit */
    uint8_t  mic_flags;                     /* MIC_FLAG_* (beamform append, arm hardware) */
    uint8_t  mic_hw_armed;                  /* 1 = capture + tap are live */
    uint16_t mic_rate_hz_div;               /* requested sample rate, units of 100 Hz (160 = 16 kHz) */
    uint16_t mic_bitrate_100;               /* LC3 target bitrate, units of 100 bps (0 = default) */
    uint32_t mic_frames;                    /* stream frames emitted since session start */
    uint32_t mic_lease_deadline;            /* FW_MS_TICK streaming-lease deadline; 0 = none */
    uint32_t mic_watchdog_timer;            /* one-shot osTimer tearing down a lapsed session */
    uint8_t  mic_notify_buf[32];            /* stable storage for the field-104 sid-0x09 notify */
    /* --- Damage settings extension (damage_ext.c; Damage FIRMWARE.md §0/§3, draft).
     * Appended at the tail so every existing field offset is unchanged. --- */
    uint32_t dmg_flags;                     /* flags armed by the phone; cleared with the lease */
    uint8_t  dmg_status;                    /* the status register (FIRMWARE.md §1.2): the last recording op's */
    uint8_t  dmg_reply_buf[192];            /* stable storage for the field-111 telemetry reply */
    /* F1.3: the panel-transfer stamp. display_copy_hook marks a direct copy; the
     * refresh wrapper consumes the mark and times the stock refresh call. */
    uint8_t  dmg_direct_presented;          /* a direct frame was copied; the next refresh is its transfer */
    uint32_t dmg_present_seq;               /* direct frames copied into the framebuffer */
    uint32_t dmg_last_transfer_us;          /* the panel transfer that followed the last direct copy */
    uint8_t  dmg_notify_buf[64];            /* stable storage for the field-113 presented notify */
    /* F1.5: cache-keep. The CACHE_KEEP flag, read when the flags clear at a lease
     * lapse or release, is latched here for the fresh acquire that follows. */
    uint8_t  dmg_cache_keep_latched;
    uint8_t  dmg_lease_settled;             /* the current lapse or release has been settled once */
    uint32_t dmg_cache_gen;                 /* mode-12 writes that changed the cache since boot */
    /* The self-test (mode 16): a scratch shadow the drawing ops run against with
     * presents suppressed, its own frame-order diagnostics, and the last step's result. */
    /* The three below are written and read from different tasks — a step on the EvenHub
     * task, a release from the settings task or the input thread — so they are volatile:
     * the mark-then-read in damage_self_test and the check-then-free in
     * damage_self_test_release keep their program order. The window that remains between
     * the two is the one the installed firmware already has for its texture cache. */
    uint8_t *volatile dmg_st_shadow;        /* PANEL_BYTES from heap 13, or 0 */
    volatile uint8_t dmg_st_active;         /* a step is running: present_shadow returns without publishing */
    volatile uint8_t dmg_st_free_pending;   /* a release arrived from another task during a step: the step's epilogue frees */
    uint8_t  dmg_st_refused;                /* the last step's message was refused */
    uint32_t dmg_st_seq;                    /* steps run since the begin */
    uint32_t dmg_st_crc;                    /* CRC-32 of the scratch shadow after the last step */
    damage_diag_state dmg_st_diag;          /* the self-test's fid ring and flags (swapped in per step) */
    /* --- Damage drawing contract v2 (damage_draw.c; Damage FIRMWARE.md §4, Phase 2).
     * Appended at the tail so every earlier field offset is unchanged. --- */
    uint32_t dmg_cache_bytes;               /* op 5 CACHE_SIZE: the size the next allocation takes (0 = the
                                             * 64 KiB default); the allocated size while the cache is up;
                                             * reverts to 0 when the cache is released */
    uint8_t  dmg_ref_seen;                  /* fields 23-25: an image-lane refusal has been recorded */
    uint8_t  dmg_ref_mode;                  /* the refused message's mode byte, as received */
    uint8_t  dmg_ref_reason;                /* DMG_REF_* */
    uint8_t  dmg_last_path;                 /* the last Damage transfer: 0 the full refresh, 1 the partial rows (mode 24) */
    uint32_t dmg_ref_seq;                   /* dmg_present_seq when the refusal was recorded */
    damage_slot dmg_slots[DMG_SLOTS];       /* save-under (mode 23): the live session's slots */
    damage_slot dmg_st_slots[DMG_SLOTS];    /* the self-test's slots, swapped in for a step */
    uint32_t dmg_slot_bytes;                /* bytes the live slots hold, against DMG_SAVE_BUDGET */
    uint32_t dmg_st_slot_bytes;
    /* Mode 24, the present hint: queued with the direct job under the display gate
     * (present_shadow), latched by the copy hook for the refresh that follows it, consumed
     * by damage_refresh_hook. Two copies, so a later job's hint cannot reach an earlier
     * job's refresh (the gate is given back between the copy and the refresh). */
    uint8_t  dmg_hint_q_on;
    uint8_t  dmg_hint_r_on;
    uint16_t dmg_hint_q_y0, dmg_hint_q_y1;
    uint16_t dmg_hint_r_y0, dmg_hint_r_y1;
} customCfwContext;

#define CFW_CTX_SLOT  0x202a6270U    /* first word of the CFW-reserved TLSF tail */
#define CFW_ALLOC_DIAG_SLOT 0x202a6274U /* second word: magic | sticky failure bit */
#define CFW_ALLOC_DIAG_MAGIC 0xA110CA7EU
#define CFW_CTX_MAGIC 0xC0FFEE6BU    /* bumped for the context layout change (Damage Phase 2 fields) */

#define FW_MS_TICK  (*(volatile uint32_t *)0x20074a34U)  /* firmware 1 ms OS tick (SysTick chain) */

static customCfwContext *peekCustomCfwContext(void);
static customCfwContext *getCustomCfwContext(void);
int cfw_fb_lease_active(void);
void damage_clear_flags(customCfwContext *ctx);          /* damage_ext.c */
void damage_lease_ended(customCfwContext *ctx);          /* damage_ext.c: a lapse noticed, or FB_RELEASE */
void damage_lease_fresh_acquire(customCfwContext *ctx);  /* damage_ext.c: FB_ACQUIRE with no live lease */
void damage_session_cleanup(customCfwContext *ctx);      /* damage_ext.c: mode 11 */
int  damage_self_test(const uint8_t *src, uint32_t srclen);   /* damage_ext.c: image mode 16 */
/* damage_draw.c: the drawing contract v2 (Damage FIRMWARE.md §4) */
void damage_refuse(const uint8_t *src, unsigned reason);      /* record an image-lane refusal (fields 23-25) */
void damage_clear_refusal(customCfwContext *ctx);             /* mode 7 sub 0 */
uint32_t damage_cache_size(customCfwContext *ctx);            /* the texture cache's size for this session */
int  damage_draw2_armed(customCfwContext *ctx);               /* flag bit 2 */
int  damage_dispatch_v2(uint8_t *state, const uint8_t *src, uint32_t srclen, int present, void *rl);
void damage_slots_free(damage_slot *slots, uint32_t *bytes);  /* free every slot of a set */
void damage_slots_swap(customCfwContext *ctx);                /* the self-test's set in, the live one out (and back) */
