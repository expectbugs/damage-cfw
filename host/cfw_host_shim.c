/*
 * x86 host harness for the patch sources (host/README.md).
 *
 * Context: the Damage build of the G2 custom firmware (../DAMAGE.md) is compiled
 * for the glasses with clang into one appended code block. To check its drawing
 * behaviour on this PC, the same sources are compiled for 32-bit x86 and run here
 * against conformance vectors — message sequences with the expected shadow CRC per
 * lens — that Damage's Kotlin simulator runs too (Damage FIRMWARE.md §9).
 *
 * How the firmware's fixed addresses are honoured on the host:
 *   - the sources call firmware functions through constant addresses and read RAM
 *     at constant addresses; this file maps those address ranges at their exact
 *     values (MAP_FIXED_NOREPLACE, 32-bit process) before anything runs;
 *   - at each firmware function address it writes a 7-byte x86 jump
 *     (`mov eax, imm32; jmp eax`) to a host function with the same prototype;
 *   - the host functions model only what the drawing path needs: zlib inflate is
 *     the host's zlib (the firmware carries zlib 1.1.4; the output of a valid
 *     stream is the same, error paths on damaged streams may differ), the display
 *     task's refresh runs synchronously (the hook copies the shadow to a host
 *     framebuffer, then the gate is given back), timers, sound, radio and sensors
 *     are counted and otherwise do nothing.
 *
 * Commands on stdin, one per line; answers on stdout:
 *   lens L|R         which lens this process models (FW_SIDE: 2 = left, 1 = right)
 *   tick N           set the firmware millisecond tick
 *   control HEX      a sid-0x09 field-101 body (the lease ops)
 *   msg HEX          one completed image message: into buffer B, snapshot, deferred
 *                    handler — the firmware's own entry points — answers "rc N"
 *   crc              answers "crc SHADOW FB presents P gate TAKES/GIVES"
 *   flags            answers "flags reorder skip dup snapof alloc"
 *   settings HEX     a whole sid-0x09 request payload through settings_decode_wrapper
 *   respond HEX      a stock settings response body through settings_send_wrapper
 *   dmg              answers "dmg FLAGS STATUS GEN LATCHED PRESENTS TRANSFER_US ST_SEQ ST_REFUSED
 *                    ST_CRC CACHE" — the Damage extension's state (damage_ext.c), read straight
 *                    from the context: what the left lens holds but cannot report
 * Every message the patch code sends answers "send TYPE SID HEX" — from the RIGHT lens
 * only: the stock senders refuse on the left lens (FUN_00475b14 -> FUN_0046f258 -> 8), and
 * so does h_send, so a test sees what the phone would.
 *
 * The display task's refresh (FUN_004ca564, the sixth firmware call a type-3 refresh
 * makes) is modeled by h_panel_refresh: it counts, and advances the DWT cycle counter by
 * 1,234 µs worth of the primed 250,000 cycles/ms, so the F1.3 transfer stamp is a
 * known number here (0 for the copy and the worker, whose regions see no cycles).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <zlib.h>

/* ---- the doors into the patch unit (cfw_host_patches.c) ---- */
void host_settings_control(const uint8_t *data, uint32_t len);
void host_context_prime(void);
int host_flags(uint8_t *out);
int cfw_snapshot(uint8_t *state, uint32_t container_id);
int image_deferred(uint8_t *state, uint8_t *src, uint32_t len);
void display_copy_hook(void);
int settings_decode_wrapper(void *stream, const void *fields, void *dest);
int settings_send_wrapper(int type, int sid, unsigned char *buf, unsigned len);
int damage_refresh_hook(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f);
void host_damage_state(uint32_t *out);

/* ---- fixed regions (addresses from the patch sources) ---- */
#define CODE_BASE   0x00438000u   /* stock main app load address */
#define CODE_LEN    0x003C8000u   /* up to 0x00800000 */
#define RAM_BASE    0x20000000u
#define RAM_LEN     0x00400000u   /* covers 0x2034dc30, the highest RAM address used */
#define DWT_PAGE    0xE0001000u
#define DEBUG_PAGE  0xE000E000u

/* host-chosen places inside the RAM mapping for the modeled container */
#define BUF_A       0x20100000u   /* display buffer A: the 640x480 packed shadow lives here */
#define BUF_B       0x20130000u   /* reconstruction buffer B */
#define HOST_FB     0x20160000u   /* the physical framebuffer the hook copies into */
#define POOL_BASE   0x20300000u   /* FW_MALLOC's pool (the context must sit in firmware SRAM) */
#define POOL_END    0x20340000u
#define CARRIER_BYTES 165888u     /* 576 x 288, the EvenHub carrier's allocation */

#define FW_MS_TICK_ADDR   0x20074a34u
#define FW_DISPLAY_FB_PTR 0x200007b8u
#define ZLIB_VER_ADDR     0x0078d654u

static uint32_t lens_side = 1;          /* FW_SIDE(): 1 right, 2 left */
static unsigned presents, gate_takes, gate_gives, bmp_calls, stock_copies, panel_refreshes;
static uint32_t pool_next = POOL_BASE;
static uint32_t timer_handles;
static uint8_t state[0x48];

static void die(const char *what) {
    fprintf(stderr, "cfw_host: %s (errno %d: %s)\n", what, errno, strerror(errno));
    exit(2);
}

static void map_fixed(uint32_t base, uint32_t len, int prot) {
    void *p = mmap((void *)(uintptr_t)base, len, prot,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED || (uintptr_t)p != base) {
        char msg[96];
        snprintf(msg, sizeof msg, "cannot map 0x%08x+0x%x at its fixed address", base, len);
        die(msg);
    }
}

/* ---- host functions standing in for firmware entry points ---- */
static int h_inflate_init2(void *strm, int bits, const char *ver, int size) {
    (void)ver; (void)size;   /* the firmware passes "1.1.4" and 0x38; the host zlib wants its own */
    return inflateInit2_((z_streamp)strm, bits, ZLIB_VERSION, (int)sizeof(z_stream));
}
static int h_inflate(void *strm, int flush) { return inflate((z_streamp)strm, flush); }
static int h_inflate_end(void *strm) { return inflateEnd((z_streamp)strm); }
static int h_loadbmp(void *s, void *b, uint32_t l) { (void)s; (void)b; (void)l; bmp_calls++; return -1; }
static void h_flush(void *d) { (void)d; }
static void h_set_src(uint32_t o, void *d) { (void)o; (void)d; }
static void h_invalidate(uint32_t o) { (void)o; }
static uint32_t h_side(void) { return lens_side; }
static void h_buzz1(uint32_t a) { (void)a; }
static void h_buzz3(uint32_t a, uint32_t b, uint32_t c) { (void)a; (void)b; (void)c; }
static void h_void(void) {}
static void h_buzz_raw(uint32_t f, uint32_t d) { (void)f; (void)d; }
static int h_timer_start(uint32_t h, uint32_t ms) { (void)h; (void)ms; return 0; }
static uint32_t h_timer_new(void *cb, uint32_t t, void *a, void *at) { (void)cb; (void)t; (void)a; (void)at; return ++timer_handles; }
static int h_timer_stop(uint32_t h) { (void)h; return 0; }
static void h_app_start(unsigned id, void *a, unsigned l, void *cb) { (void)id; (void)a; (void)l; (void)cb; }
static uint8_t *h_lookup(uint32_t id) { (void)id; return 0; }
static int h_complete_emit(uint32_t id, void *hdr, int k, uint32_t p) { (void)id; (void)hdr; (void)k; (void)p; return 0; }
static void h_display_wait(void) { gate_takes++; }
static void h_display_signal(void) { gate_gives++; }
static int h_panel_refresh(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    panel_refreshes++;
    *(volatile uint32_t *)(uintptr_t)0xE0001004u += 250u * 1234u;   /* DWT CYCCNT: 1,234 µs at 250 cycles/µs */
    return 0;
}
static int h_display_queue(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t w, uint32_t hh) {
    /* the display task: FUN_00473c44 type 3 runs the copy hook, gives the gate, then
     * (panel on) calls the refresh — through damage_refresh_hook since the F1.3 site */
    presents++;
    display_copy_hook();
    gate_gives++;
    damage_refresh_hook(a, b, c, d, w, hh);
    return 0;
}
static void h_display_copy(void) { stock_copies++; }
static int h_int_void(void) { return 0; }
static int h_event_forward(uint32_t d, uint32_t e, void *v) { (void)d; (void)e; (void)v; return 0; }
static int h_compass_notify(uint32_t h) { (void)h; return 0; }
static void *h_malloc(uint32_t n) {
    uint32_t p = (pool_next + 3u) & ~3u;
    if (p + n > POOL_END) return 0;
    pool_next = p + n;
    return (void *)(uintptr_t)p;
}
static void h_free(void *p) { (void)p; }
static void *h_heap_malloc(uint32_t desc, uint32_t n) { (void)desc; return malloc(n); }
static void h_heap_free(uint32_t desc, void *p) { (void)desc; free(p); }
static int h_send(int t, int sid, unsigned char *b, unsigned l) {
    if (lens_side == 2) return 8;     /* FUN_00475b14 / FUN_00475c1a: the left lens does not send */
    printf("send %d %d ", t, sid);
    for (unsigned i = 0; i < l; i++) printf("%02x", b[i]);
    printf("\n");
    return 0;
}
static int h_pb_decode(void *s, const void *f, void *d) { (void)s; (void)f; (void)d; return 1; }
static unsigned h_wear(void) { return 2; }
static int evenhub_mode = 0xe0;
static int *h_mode(void *g) { (void)g; return &evenhub_mode; }
static int h_sysevt(int a, int b, int c, int d, int e, int f) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return 0; }
static void h_longpress(unsigned c, unsigned a) { (void)c; (void)a; }
static int h_font_dsc(const void *f, void *d, uint32_t l, uint32_t n) { (void)f; (void)d; (void)l; (void)n; return 0; }
static const uint8_t *h_font_bitmap(void *d, void *b) { (void)d; (void)b; return 0; }
static void h_font_release(void *d) { (void)d; }
static void h_mic_sel(uint32_t s) { (void)s; }
static int h_pcm_register(uint32_t s, uint32_t a, void *cb) { (void)s; (void)a; (void)cb; return -1; }
static int h_pcm_unregister(uint32_t s, uint32_t a) { (void)s; (void)a; return -1; }
static void h_algo(const void *p, int16_t *s, int16_t *a) { (void)p; (void)s; (void)a; }
static int h_audio_notify(const void *b, uint32_t l) { (void)b; (void)l; return -1; }

struct jump { uint32_t addr; void *to; };
static const struct jump jumps[] = {
    {0x005beac3u, h_inflate_init2}, {0x005beb91u, h_inflate}, {0x005bea87u, h_inflate_end},
    {0x004dc5afu, h_loadbmp}, {0x0047510fu, h_flush}, {0x00498681u, h_set_src}, {0x00440657u, h_invalidate},
    {0x0045a569u, h_side},
    {0x00502b5bu, h_buzz1}, {0x00502bf9u, h_buzz3}, {0x00502ac5u, h_void}, {0x00502c89u, h_buzz_raw},
    {0x00449499u, h_timer_start}, {0x004493b1u, h_timer_new}, {0x004494d9u, h_timer_stop}, {0x0044953fu, h_timer_stop},
    {0x00464b2fu, h_app_start}, {0x004e0cbbu, h_void}, {0x004e0ccfu, h_lookup}, {0x004da383u, h_complete_emit},
    {0x0047381fu, h_display_wait}, {0x0047386bu, h_display_signal}, {0x00474067u, h_display_queue},
    {0x0046ca15u, h_display_copy}, {0x004ca565u, h_panel_refresh},
    {0x005455e5u, h_int_void}, {0x0054566du, h_int_void}, {0x0045f8fdu, h_event_forward}, {0x0058705du, h_compass_notify},
    {0x00474cd3u, h_malloc}, {0x00474d17u, h_free}, {0x00484181u, h_heap_malloc}, {0x0048429fu, h_heap_free},
    {0x00475b15u, h_send}, {0x00475c1bu, h_send}, {0x00490121u, h_pb_decode}, {0x0049eb8fu, h_wear},
    {0x0045f8e7u, h_mode}, {0x004da16bu, h_sysevt}, {0x0046ae9du, h_longpress},
    {0x004d56c1u, h_font_dsc}, {0x004d5613u, h_font_bitmap}, {0x004d5665u, h_font_release},
    {0x0058f69bu, h_mic_sel}, {0x0058f74bu, h_void}, {0x0058f7b1u, h_mic_sel}, {0x0058f807u, h_void},
    {0x0057ab79u, h_pcm_register}, {0x0057acd1u, h_pcm_unregister}, {0x00591bfdu, h_algo}, {0x00475d79u, h_audio_notify},
};

static void install(void) {
    map_fixed(CODE_BASE, CODE_LEN, PROT_READ | PROT_WRITE | PROT_EXEC);
    map_fixed(RAM_BASE, RAM_LEN, PROT_READ | PROT_WRITE);
    map_fixed(DWT_PAGE, 0x1000, PROT_READ | PROT_WRITE);
    map_fixed(DEBUG_PAGE, 0x1000, PROT_READ | PROT_WRITE);
    size_t n = sizeof jumps / sizeof jumps[0];
    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            uint32_t a = jumps[i].addr, b = jumps[j].addr;
            if (i != j && a <= b && b - a < 7) die("two firmware entry points closer than a 7-byte jump");
        }
        uint8_t *p = (uint8_t *)(uintptr_t)jumps[i].addr;
        uint32_t to = (uint32_t)(uintptr_t)jumps[i].to;
        p[0] = 0xB8; memcpy(p + 1, &to, 4); p[5] = 0xFF; p[6] = 0xE0;   /* mov eax, to; jmp eax */
    }
    memcpy((void *)(uintptr_t)ZLIB_VER_ADDR, "1.1.4", 6);
    *(uint32_t *)(uintptr_t)FW_DISPLAY_FB_PTR = HOST_FB;
    *(uint8_t **)(uintptr_t)0x200744d0u = (uint8_t *)(uintptr_t)0x20000100u;   /* UI_CTX: any non-null */
    *(uint8_t *)(uintptr_t)0x2034dc30u = 4;                                    /* EVT_SRC: the ring */
    /* the modeled image container (zlib_glue.c: state+0x8 A, +0xc B, +0x18 CompressMode,
     * +0x20 length, +0x40/+0x42 carrier size, +0x44 capacity) */
    uint32_t a = BUF_A, b = BUF_B, cap = CARRIER_BYTES;
    uint16_t w = 576, h = 288;
    memcpy(state + 0x08, &a, 4); memcpy(state + 0x0c, &b, 4);
    memcpy(state + 0x40, &w, 2); memcpy(state + 0x42, &h, 2); memcpy(state + 0x44, &cap, 4);
}

static int unhex(const char *s, uint8_t *out, size_t cap, size_t *n) {
    size_t len = strlen(s), k = 0;
    while (len && (s[len - 1] == '\n' || s[len - 1] == '\r')) len--;
    if (len % 2) return 0;
    for (size_t i = 0; i < len; i += 2) {
        unsigned v;
        if (k >= cap || sscanf(s + i, "%2x", &v) != 1) return 0;
        out[k++] = (uint8_t)v;
    }
    *n = k;
    return 1;
}

int main(void) {
    install();
    host_context_prime();
    static char line[1 << 20];
    static uint8_t buf[1 << 19];
    setvbuf(stdout, 0, _IOLBF, 0);
    while (fgets(line, sizeof line, stdin)) {
        size_t n;
        if (!strncmp(line, "lens ", 5)) {
            lens_side = line[5] == 'L' ? 2 : 1;
        } else if (!strncmp(line, "tick ", 5)) {
            *(uint32_t *)(uintptr_t)FW_MS_TICK_ADDR = (uint32_t)strtoul(line + 5, 0, 10);
        } else if (!strncmp(line, "control ", 8)) {
            if (!unhex(line + 8, buf, sizeof buf, &n)) { printf("error control hex\n"); continue; }
            host_settings_control(buf, (uint32_t)n);
        } else if (!strncmp(line, "msg ", 4)) {
            if (!unhex(line + 4, buf, sizeof buf, &n) || n > CARRIER_BYTES) { printf("error msg hex\n"); continue; }
            uint32_t len = (uint32_t)n, mode0 = 0;
            memcpy((void *)(uintptr_t)BUF_B, buf, n);
            memcpy(state + 0x20, &len, 4);
            memcpy(state + 0x18, &mode0, 4);
            cfw_snapshot(state, 0);
            int rc = image_deferred(state, (uint8_t *)(uintptr_t)BUF_B, len);
            printf("rc %d\n", rc);
        } else if (!strncmp(line, "settings ", 9)) {
            if (!unhex(line + 9, buf, sizeof buf, &n)) { printf("error settings hex\n"); continue; }
            uint32_t stream[4] = {0, (uint32_t)(uintptr_t)buf, (uint32_t)n, 0};   /* pb_istream_t */
            settings_decode_wrapper(stream, 0, 0);
        } else if (!strncmp(line, "respond ", 8)) {
            static unsigned char resp[256];
            if (!unhex(line + 8, resp, sizeof resp, &n)) { printf("error respond hex\n"); continue; }
            settings_send_wrapper(1, 9, resp, (unsigned)n);
        } else if (!strncmp(line, "crc", 3)) {
            uLong s = crc32(0L, (const Bytef *)(uintptr_t)BUF_A, 153600u);
            uLong f = crc32(0L, (const Bytef *)(uintptr_t)HOST_FB, 153600u);
            printf("crc %08lx %08lx presents %u gate %u/%u\n", s, f, presents, gate_takes, gate_gives);
        } else if (!strncmp(line, "flags", 5)) {
            uint8_t fl[5] = {0};
            host_flags(fl);
            printf("flags %u %u %u %u %u\n", fl[0], fl[1], fl[2], fl[3], fl[4]);
        } else if (!strncmp(line, "dmg", 3)) {
            uint32_t d[10] = {0};
            host_damage_state(d);
            printf("dmg %u %u %u %u %u %u %u %u %08x %u\n", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9]);
        } else if (line[0] != '\n' && line[0] != '#') {
            printf("error unknown command\n");
        }
    }
    return 0;
}
