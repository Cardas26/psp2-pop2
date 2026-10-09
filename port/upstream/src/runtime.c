#include <stdio.h>
#include <time.h>
#include <ctype.h>
#include "plat.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "cpu.h"
#include "dispatch.h"
#include "sha256.h"
#include <sys/stat.h>
#include "levers.h"

#define LOADSEG 0x0100
#define PSPSEG  (LOADSEG - 0x10)
#define BIOSSEG 0xF000
#define MEMTOP  0x9FFF

Reg rA, rB, rC, rD;
uint16_t rSI, rDI, rBP, rSP, sDS, sES, sSS;
uint8_t FC, FZ, FS, FO, FP, FA, FD, FI;
uint8_t *MEM;
const uint8_t parity_tab[256] = {
#define P2(n) n, n^1, n^1, n
#define P4(n) P2(n), P2(n^1), P2(n^1), P2(n)
#define P6(n) P4(n), P4(n^1), P4(n^1), P4(n)
    P6(1), P6(0), P6(0), P6(1)
};

#define TIMED(field, stmt) do { stmt; } while (0)
void logmsg(const char *fmt, ...) {
    char buf[2048]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    TIMED(log_us, plat_log_write(buf));
}
static void fatal(const char *fmt, ...) {
    char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    logmsg("FATAL: %s\n", buf);
    plat_fatal_box("PoP2 native - fatal", buf);
    exit(1);
}
#define PATH_FMT(buf, ...) do { if (snprintf(buf, MAX_PATH, __VA_ARGS__) >= MAX_PATH) logmsg("path too long: %s\n", buf); } while (0)
static int ieq(const char *a, const char *b) { for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0; return *a == *b; }
static int ineq(const char *a, const char *b, size_t n) { for (size_t i = 0; i < n; i++) { if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0; if (!a[i]) break; } return 1; }
static const char *base_name(const char *p) { const char *b = p; for (const char *q = p; *q; q++) if (*q == '\\' || *q == '/') b = q + 1; return b; }

void rt_divide_error(void) {
    logmsg("divide error in %s: ax=%04x dx=%04x bx=%04x cx=%04x si=%04x di=%04x ss:sp=%04x:%04x bp=%04x ds=%04x\n", cur_fn, AX, DX, BX, CX, SI, DI, SS, SP, BP, DS);
    char line[400]; int n = 0;
    for (int o = -26; o <= 10; o += 2) n += snprintf(line + n, sizeof line - (size_t)n, " [bp%+d]=%04x", o, RW(SS, (uint16_t)(BP + o)));
    logmsg("   frame:%s\n", line);
    uint16_t po = RW(SS, (uint16_t)(BP + 6)), ps = RW(SS, (uint16_t)(BP + 8)); n = 0;
    for (int i = 0; i < 8; i++) n += snprintf(line + n, sizeof line - (size_t)n, " %04x", RW(ps, (uint16_t)(po + 2 * i)));
    logmsg("   at %04x:%04x:%s\n", ps, po, line);
    do_int(0, 0, 0);
}
void op_daa(void) { uint8_t a = AL; if ((a & 15) > 9 || FA) { AL += 6; FA = 1; } else FA = 0; if (a > 0x99 || FC) { AL += 0x60; FC = 1; } else FC = 0; SZP8(AL); }
void op_das(void) { uint8_t a = AL; if ((a & 15) > 9 || FA) { AL -= 6; FA = 1; } else FA = 0; if (a > 0x99 || FC) { AL -= 0x60; FC = 1; } else FC = 0; SZP8(AL); }
void op_aaa(void) { if ((AL & 15) > 9 || FA) { AX += 0x106; FA = FC = 1; } else FA = FC = 0; AL &= 15; }
void op_aas(void) { if ((AL & 15) > 9 || FA) { AX -= 6; AH -= 1; FA = FC = 1; } else FA = FC = 0; AL &= 15; }
void op_aam(void) { AH = AL / 10; AL = AL % 10; SZP8(AL); }
void op_aad(void) { AL = (uint8_t)(AH * 10 + AL); AH = 0; SZP8(AL); }
void fpu_esc(int op) { static int n; if (n++ < 20) logmsg("fpu esc %x ignored\n", op); }

#define STEP(sz) (FD ? -(sz) : (sz))
void str_movsb(uint16_t seg, int rep) { do { if (rep && !CX) break; WB(ES, DI, RB(seg, SI)); SI += STEP(1); DI += STEP(1); if (rep) CX--; } while (rep); }
void str_movsw(uint16_t seg, int rep) { do { if (rep && !CX) break; WW(ES, DI, RW(seg, SI)); SI += STEP(2); DI += STEP(2); if (rep) CX--; } while (rep); }
void str_stosb(uint16_t seg, int rep) { (void)seg; do { if (rep && !CX) break; WB(ES, DI, AL); DI += STEP(1); if (rep) CX--; } while (rep); }
void str_stosw(uint16_t seg, int rep) { (void)seg; do { if (rep && !CX) break; WW(ES, DI, AX); DI += STEP(2); if (rep) CX--; } while (rep); }
void str_lodsb(uint16_t seg, int rep) { do { if (rep && !CX) break; AL = RB(seg, SI); SI += STEP(1); if (rep) CX--; } while (rep); }
void str_lodsw(uint16_t seg, int rep) { do { if (rep && !CX) break; AX = RW(seg, SI); SI += STEP(2); if (rep) CX--; } while (rep); }
void str_cmpsb(uint16_t seg, int rep) { do { if (rep && !CX) break; op_sub8(RB(seg, SI), RB(ES, DI)); SI += STEP(1); DI += STEP(1); if (rep) { CX--; if ((rep == 1 && !FZ) || (rep == 2 && FZ)) break; } } while (rep); }
void str_cmpsw(uint16_t seg, int rep) { do { if (rep && !CX) break; op_sub16(RW(seg, SI), RW(ES, DI)); SI += STEP(2); DI += STEP(2); if (rep) { CX--; if ((rep == 1 && !FZ) || (rep == 2 && FZ)) break; } } while (rep); }
void str_scasb(uint16_t seg, int rep) { (void)seg; do { if (rep && !CX) break; op_sub8(AL, RB(ES, DI)); DI += STEP(1); if (rep) { CX--; if ((rep == 1 && !FZ) || (rep == 2 && FZ)) break; } } while (rep); }
void str_scasw(uint16_t seg, int rep) { (void)seg; do { if (rep && !CX) break; op_sub16(AX, RW(ES, DI)); DI += STEP(2); if (rep) { CX--; if ((rep == 1 && !FZ) || (rep == 2 && FZ)) break; } } while (rep); }

typedef struct { uint32_t lin; uint16_t seg; Fn fn; } LinFn;
typedef struct { const SpaceTab *t; LinFn *fns; int n; uint32_t lo, hi; } Space;
static Space spaces[32]; static int nspaces;
static uint8_t para_ovl[0x10000];

static int cmp_linfn(const void *a, const void *b) {
    uint32_t x = ((const LinFn *)a)->lin, y = ((const LinFn *)b)->lin; return x < y ? -1 : x > y;
}
static void init_dispatch(void) {
    for (const SpaceTab *t = space_tabs; t->name; t++) {
        Space *s = &spaces[nspaces++];
        s->t = t;
        int n = 0; while (t->tab[n].fn) n++;
        s->fns = malloc(sizeof(LinFn) * (n + 1)); s->n = n;
        for (int i = 0; i < n; i++) { s->fns[i].lin = LIN(t->tab[i].seg, t->tab[i].off); s->fns[i].seg = t->tab[i].seg; s->fns[i].fn = t->tab[i].fn; }
        qsort(s->fns, n, sizeof(LinFn), cmp_linfn);
        s->lo = (uint32_t)t->seg << 4; s->hi = s->lo + t->size;
    }
}
static Fn find_in_seg(Space *s, uint32_t lin, uint16_t seg) {
    int lo = 0, hi = s->n - 1, m = -1;
    while (lo <= hi) { int k = (lo + hi) / 2; if (s->fns[k].lin == lin) { m = k; break; } if (s->fns[k].lin < lin) lo = k + 1; else hi = k - 1; }
    if (m < 0) return NULL;
    while (m > 0 && s->fns[m - 1].lin == lin) m--;
    for (int k = m; k < s->n && s->fns[k].lin == lin; k++) if (s->fns[k].seg == seg) return s->fns[k].fn;
    return s->fns[m].fn;
}
static void record_missing(const char *sp, uint32_t lin, uint16_t seg, uint16_t off) {
    FILE *f = fopen("missing_entries.txt", "a");
    if (f) { fprintf(f, "%s %04x:%04x\n", sp, (seg - LOADSEG) & 0xFFFF, off); fclose(f); }
    logmsg("missing code: last function %s  ss:sp=%04x:%04x bp=%04x ds=%04x es=%04x ax=%04x\n", cur_fn, SS, SP, BP, DS, ES, AX);
    fatal("no recompiled code for %04x:%04x (space %s, linear %05x)", seg, off, sp, lin);
}
static void bios_call(uint16_t off);
static void write_overlay_dump(const SpaceTab *t, uint16_t seg, uint16_t off) {
    char dir[MAX_PATH], path[MAX_PATH];
    const char *e = getenv("POP2_OVLDIR");
    if (e) snprintf(dir, MAX_PATH, "%s", e);
    else { plat_mkdir("ovl"); snprintf(dir, MAX_PATH, "ovl/%s", pop2_version); }
    plat_mkdir(dir);
    PATH_FMT(path, "%s/%s.bin", dir, t->name);
    const uint8_t *img = MEM + ((uint32_t)t->seg << 4);
    FILE *f = fopen(path, "wb");
    if (!f) fatal("cannot write %s", path);
    fwrite(img, 1, t->memsize, f); fclose(f);
    logmsg("overlay %s (id %d) dumped: %s, %d bytes from %05x, first call %04x:%04x, opens %02x %02x %02x %02x\n",
           t->name, t->ovl_id, path, t->memsize, (uint32_t)t->seg << 4, seg, off, img[0], img[1], img[2], img[3]);
    fprintf(stderr, "overlay %s dumped to %s: rerun recomp.py and rebuild\n", t->name, path);
}
static void dump_overlay(const SpaceTab *t, uint16_t seg, uint16_t off) { write_overlay_dump(t, seg, off); exit(3); }
static void call_dispatch(uint16_t seg, uint16_t off);
static void dump_all_overlays(void) {
    static int done; if (done) return; done = 1;
    Space *root = &spaces[0];
    for (int i = 0; i < nspaces; i++) if (spaces[i].t->ovl_id == 0) { root = &spaces[i]; break; }
    int n = 0;
    for (int i = 0; i < nspaces; i++) {
        const SpaceTab *t = spaces[i].t;
        if (!(t->flags & OVL_COMPRESSED) || t->fp) continue;
        int found = 0;
        for (uint32_t a = root->lo; a + 10 <= root->hi && !found; a++) {
            const uint8_t *p = MEM + a;
            if (p[0] != 0xE8 || p[3] != 0xEA || p[9] != 0 || p[8] != t->ovl_id - 2 || (p[6] | p[7] << 8) != t->seg) continue;
            uint32_t mgr = a + 3 + (int16_t)(p[1] | p[2] << 8);
            for (int k = 0; k < root->n && !found; k++) {
                if (root->fns[k].lin != mgr) continue;
                uint16_t cs = root->fns[k].seg, ret = (uint16_t)(a + 3 - ((uint32_t)cs << 4));
                uint16_t sp0 = SP; PUSH(ret); call_dispatch(cs, (uint16_t)(mgr - ((uint32_t)cs << 4))); SP = sp0;
                logmsg("POP2_DUMP_ALL: %s loaded through the stub at %04x:%04x (manager %04x:%04x)\n", t->name, cs, ret - 3, cs, (uint16_t)(mgr - ((uint32_t)cs << 4)));
                write_overlay_dump(t, cs, ret); n++; found = 1;
            }
        }
        if (!found) logmsg("POP2_DUMP_ALL: no RTLink stub for %s (id %d, seg %04x) in the root\n", t->name, t->ovl_id, t->seg);
    }
    logmsg("POP2_DUMP_ALL: %d overlays dumped\n", n);
    exit(n ? 3 : 0);
}

enum { DRV_NONE, DRV_DIGI, DRV_MIDI };
static uint32_t drv_base[3], drv_size[3];
static int cur_drv_kind;
static void fm_driver_check(const char *name);
static int cfg_sound_keep;

static void note_driver_read(const char *name, uint32_t dst, uint32_t fpos) {
    size_t l = strlen(name);
    if (l < 4 || !ieq(name + l - 4, ".DRV")) return;
    const char *b = base_name(name);
    int kind = ineq(b, "M", 1) ? DRV_MIDI : DRV_DIGI;
    drv_base[kind] = dst - fpos;
    { FILE *f = fopen(name, "rb"); if (f) { fseek(f, 0, SEEK_END); drv_size[kind] = (uint32_t)ftell(f); fclose(f); } }
    logmsg("driver %s read: file %u -> %05x (base %05x, kind %d)\n", name, fpos, dst, drv_base[kind], kind);
    if (kind == DRV_MIDI) fm_driver_check(name);
}
#define FM_SEG 0xD000
static int fm_mode;
static const char *checked_file(const char *name);
void fm_start(void); void fm_port_write(uint16_t port, uint8_t v); uint8_t fm_port_read(void);
static void fm_driver_check(const char *name) {
    if (getenv("POP2_NOFM")) return;
    const SpaceTab *t = NULL;
    for (int i = 0; i < nspaces; i++) if (spaces[i].t->ovl_id == 100) { t = spaces[i].t; break; }
    if (!t || fm_mode) return;
    char ref[MAX_PATH]; const char *b = base_name(name);
    snprintf(ref, sizeof ref, "%.*smsb_pro.drv", (int)(b - name), name);
    if (checked_file("msb_pro.drv")) snprintf(ref, sizeof ref, "%s", checked_file("msb_pro.drv"));
    static uint8_t a[4096], r[4096];
    FILE *f = fopen(name, "rb"), *g = fopen(ref, "rb");
    size_t na = f ? fread(a, 1, sizeof a, f) : 0, nr = g ? fread(r, 1, sizeof r, g) : 0;
    if (f) fclose(f);
    if (g) fclose(g);
    if (nr != (size_t)t->size) {
        logmsg("msb_pro.drv missing or not the recompiled version: General MIDI output\n");
        return;
    }
    if (na != nr || memcmp(a + 64, r + 64, nr - 64) != 0)
        logmsg("MIDI.DRV is not the FM driver - using the game's own msb_pro.drv (pop2.ini Music=gm for General MIDI)\n");
    memcpy(MEM + FM_SEG * 16, r, nr);
    int si = 0;
    for (int i = 0; i < nspaces; i++) if (spaces[i].t == t) si = i;
    for (uint32_t p = 0; p < nr; p += 16) para_ovl[FM_SEG + p / 16] = (uint8_t)(si + 1);
    fm_mode = 1;
    fm_start();
    logmsg("FM driver: running recompiled driver code with OPL2 emulation\n");
}
void snd_driver_call(int kind);
static int driver_dispatch(uint16_t seg, uint16_t off) {
    uint32_t lin = LIN(seg, off);
    for (int k = DRV_DIGI; k <= DRV_MIDI; k++) {
        if (drv_base[k] && lin >= drv_base[k] && lin < drv_base[k] + drv_size[k]) {
            if (k == DRV_MIDI && fm_mode) {
                int fn = AL;
                uint16_t iax = AX, ibx = BX, icx = CX, idx = DX;
                call_far(FM_SEG, (uint16_t)(lin - drv_base[k]));
                if (getenv("POP2_MIDILOG") && fn < 0x80)
                    logmsg("FMDRV fn %02x: in ax=%04x bx=%04x cx=%04x dx=%04x -> ax=%04x bx=%04x cx=%04x dx=%04x\n",
                           fn, iax, ibx, icx, idx, AX, BX, CX, DX);
                if (fn == 1) { WW(FM_SEG, 0x147, 1); WW(FM_SEG, 0x149, 1); }
                return 1;
            }
            snd_driver_call(k);
            POP(); POP();
            return 1;
        }
    }
    return 0;
}

static void call_dispatch(uint16_t seg, uint16_t off) {
    uint32_t lin = LIN(seg, off);
    if (seg == BIOSSEG) { bios_call(off); return; }
    if (driver_dispatch(seg, off)) return;
    Space *root = &spaces[0];
    for (int i = 0; i < nspaces; i++) if (spaces[i].t->ovl_id == 0) { root = &spaces[i]; break; }
    if (lin >= root->lo && lin < root->hi) {
        Fn f = find_in_seg(root, lin, seg); if (f) { f(); return; }
        record_missing("root", lin, (uint16_t)(seg), off); return;
    }
    int si = para_ovl[(lin >> 4) & 0xFFFF];
    {
        Space *match = NULL, *und = NULL, *hint = si ? &spaces[si - 1] : NULL; int ncand = 0, nund = 0;
        for (int i = 0; i < nspaces; i++) {
            const SpaceTab *t = spaces[i].t; uint32_t base = (uint32_t)t->seg << 4;
            if (!((t->flags & OVL_COMPRESSED) || t->fp) || lin < base || lin >= base + (uint32_t)t->memsize) continue;
            ncand++;
            if (t->fp) { if (!match && !memcmp(MEM + base, t->fp, fp_len) && !memcmp(MEM + base + t->fpmid, t->fp + fp_len, fp_len)) match = &spaces[i]; }
            else { nund++; und = &spaces[i]; }
        }
        if (ncand) {
            if (!match) {
                Space *d = nund == 1 ? und : (hint && !hint->t->fp && (hint->t->flags & OVL_COMPRESSED)) ? hint : NULL;
                if (!d) fatal("code at %05x (call %04x:%04x) matches no dumped overlay; %d undumped candidates, hint %s", lin, seg, off, nund, hint ? hint->t->name : "none");
                dump_overlay(d->t, seg, off);
            }
            if (!hint || hint != match) {
                uint32_t base = (uint32_t)match->t->seg << 4;
                for (uint32_t k = 0; k < (uint32_t)match->t->memsize; k += 16) para_ovl[((base + k) >> 4) & 0xFFFF] = (uint8_t)((match - spaces) + 1);
                logmsg("overlay %s resident at %05x by content (hint %s)\n", match->t->name, base, hint ? hint->t->name : "none");
            }
            Fn f = find_in_seg(match, lin, seg); if (f) { f(); return; }
            record_missing(match->t->name, lin, seg, off); return;
        }
    }
    if (si) {
        Space *s = &spaces[si - 1];
        Fn f = find_in_seg(s, lin, seg); if (f) { f(); return; }
        record_missing(s->t->name, lin, seg, off); return;
    }
    logmsg("no code: last function %s  ss:sp=%04x:%04x bp=%04x ds=%04x es=%04x ax=%04x bx=%04x si=%04x di=%04x\n", cur_fn, SS, SP, BP, DS, ES, AX, BX, SI, DI);
    fatal("far call to %04x:%04x: no code loaded there (in %s)", seg, off, cur_fn);
}
static void bp_check(uint16_t seg, uint16_t off, uint16_t bp0) {
    static int n;
    uint32_t a = LIN(seg, off);
    if (!(MEM[a] == 0x55 && MEM[a + 1] == 0x8B && MEM[a + 2] == 0xEC)) return;
    if (BP == bp0 || n >= 30) return;
    n++;
    logmsg("BP CLOBBER: call %04x:%04x changed bp %04x -> %04x (in %s)\n", seg, off, bp0, BP, cur_fn);
}
static void sp_check(uint16_t seg, uint16_t off, uint16_t sp0, int expect) {
    static int n;
    if (SP == (uint16_t)(sp0 + expect) || n >= 40) return;
    logmsg("STACK LEAK: call %04x:%04x returned sp %04x, expected %04x (%+d) in %s\n",
           seg, off, SP, (uint16_t)(sp0 + expect), (int16_t)(SP - (uint16_t)(sp0 + expect)), cur_fn);
    n++;
}
void call_far(uint16_t seg, uint16_t off) {
    static int chk = -1; if (chk < 0) chk = getenv("POP2_SPCHK") != NULL;
    uint16_t sp0 = SP, bp0 = BP;
    call_dispatch(seg, off);
    if (chk) { sp_check(seg, off, sp0, 4); bp_check(seg, off, bp0); }
}
void call_near(uint16_t cs, uint16_t off) {
    static int chk = -1; if (chk < 0) chk = getenv("POP2_SPCHK") != NULL;
    uint16_t sp0 = SP, bp0 = BP;
    call_dispatch(cs, off);
    if (chk) { sp_check(cs, off, sp0, 2); bp_check(cs, off, bp0); }
}
void rt_halt(uint16_t cs, uint16_t ip) { fatal("HLT at %04x:%04x", cs, ip); }
void rt_unimpl(uint16_t cs, uint16_t ip) { fatal("unimplemented instruction at %04x:%04x", cs, ip); }
void rt_badjump(uint16_t cs, uint16_t ip, uint16_t t) { fatal("jump table at %04x:%04x: unexpected target %04x", cs, ip, t); }
void rt_missing(const char *sp, uint16_t seg, uint16_t off) {
    FILE *f = fopen("missing_entries.txt", "a");
    if (f) { fprintf(f, "%s %04x:%04x\n", sp, seg, off); fclose(f); }
    fatal("missing function %s %04x:%04x", sp, seg, off);
}

static void note_code_read(uint32_t dst, uint32_t fpos, uint32_t len) {
    for (int i = 0; i < nspaces; i++) {
        const SpaceTab *t = spaces[i].t;
        if (t->ovl_id == 0 || t->ovl_id >= 100) continue;
        uint32_t a = t->fileoff, b = t->fileoff + t->size;
        if (fpos + len <= a || fpos >= b) continue;
        if (t->flags & OVL_COMPRESSED) {
            uint32_t base = (uint32_t)t->seg << 4;
            for (uint32_t k = 0; k < (uint32_t)t->memsize; k += 16) para_ovl[((base + k) >> 4) & 0xFFFF] = (uint8_t)(i + 1);
            logmsg("overlay %s (id %d) compressed: %u bytes read to %05x, unpacks at %05x..%05x\n",
                   t->name, t->ovl_id, len, dst, base, base + t->memsize);
            continue;
        }
        for (uint32_t k = 0; k < len; k += 16) {
            uint32_t fp = fpos + k;
            if (fp >= a && fp < b) para_ovl[((dst + k) >> 4) & 0xFFFF] = (uint8_t)(i + 1);
        }
        logmsg("overlay %s (id %d) loaded at %05x\n", t->name, t->ovl_id, dst - (fpos - a));
    }
}

static uint8_t pal[256][3]; static int pal_idx, pal_comp, pal_read_idx, pal_read_comp;
static uint8_t frame[320 * 200];
static int video_mode = 3;
static int cfg_filter = 1, cfg_aspect = 0;
static int plat_ready;
static int in_wait;
static int write_bmp(const char *fn) {
    FILE *f = fopen(fn, "wb"); if (!f) return 0;
    uint8_t h[54] = {0}; uint32_t off = 54 + 1024, size = off + 64000, v;
    h[0] = 'B'; h[1] = 'M'; memcpy(h + 2, &size, 4); memcpy(h + 10, &off, 4);
    v = 40; memcpy(h + 14, &v, 4); v = 320; memcpy(h + 18, &v, 4); v = 200; memcpy(h + 22, &v, 4);
    h[26] = 1; h[28] = 8; v = 64000; memcpy(h + 34, &v, 4);
    fwrite(h, 1, 54, f);
    for (int i = 0; i < 256; i++) { uint8_t q[4] = { (uint8_t)(pal[i][2] << 2 | pal[i][2] >> 4), (uint8_t)(pal[i][1] << 2 | pal[i][1] >> 4), (uint8_t)(pal[i][0] << 2 | pal[i][0] >> 4), 0 }; fwrite(q, 1, 4, f); }
    for (int y = 199; y >= 0; y--) fwrite(frame + y * 320, 320, 1, f);
    fclose(f); return 1;
}
static void present(void) {
    if (!plat_ready) return;
    memcpy(frame, MEM + 0xA0000, sizeof frame);
    int unchanged;
    {
        static uint8_t last[sizeof frame], lpal[sizeof pal];
        unchanged = !memcmp(last, frame, sizeof frame) && !memcmp(lpal, pal, sizeof pal);
        if (!unchanged) {
            memcpy(last, frame, sizeof frame); memcpy(lpal, pal, sizeof pal);
        }
    }
    {
        static uint32_t last; static int shotn; static char *dir; static int init;
        if (!init) { dir = getenv("POP2_SHOTS"); init = 1; }
        if (dir && plat_ms() - last > 1000) {
            last = plat_ms();
            char fn[MAX_PATH]; snprintf(fn, MAX_PATH, "%s/shot%03d.bmp", dir, shotn++);
            write_bmp(fn);
        }
    }
    {
        static uint64_t t_swap; uint64_t now = plat_us();
        if (unchanged && lever_on(LEVER_PRESENT) && !(in_wait && now - t_swap >= 100000)) return;
        t_swap = now;
    }
    plat_present(frame, (const uint8_t (*)[3])pal, cfg_filter, cfg_aspect);
}
static void osd_say(const char *what, const char *name) { char t[48]; snprintf(t, sizeof t, "%s: %s", what, name); plat_osd(t, 3000); logmsg("%s\n", t); }
static void ini_set(const char *key, int value);
void rt_cycle_filter(void) { cfg_filter = (cfg_filter + 1) % plat_filter_modes(); plat_set_title(cfg_filter, cfg_aspect); osd_say("Filter", plat_filter_name(cfg_filter)); ini_set("Filter", cfg_filter); }
void rt_cycle_aspect(void) { cfg_aspect = (cfg_aspect + 1) % plat_aspect_modes(); plat_set_title(cfg_filter, cfg_aspect); osd_say("View", plat_aspect_name(cfg_aspect)); ini_set("Aspect", cfg_aspect); }

static uint8_t kq[256]; static int kq_head, kq_tail; static uint8_t port60;
static uint16_t bios_kbuf[64]; static int bk_head, bk_tail;
static uint8_t key_down[0x200];
static int cp_kid_dead, cp_valid, cp_restore_pending; static uint32_t cp_dead_since;
static struct { uint32_t t; int scan, up, ascii; } key_log[32]; static int key_log_n;
static struct { uint32_t t; int val; } doskey_log[32]; static int doskey_log_n;
void rt_key_event(int scan, int up, int ascii) {
    { int i = key_log_n++ & 31; key_log[i].t = plat_ms(); key_log[i].scan = scan; key_log[i].up = up; key_log[i].ascii = ascii; }
    if (getenv("POP2_KEYLOG")) logmsg("key_event %03x %s ascii %02x\n", scan, up ? "up" : "down", ascii);
    if (!up && cp_kid_dead && cp_valid && !cp_restore_pending && plat_ms() - cp_dead_since > 1200
        && (scan & 0xFF) != 0x1D && (scan & 0xFF) != 0x38) {
        cp_restore_pending = 1; key_down[scan & 0x1FF] = 1;
        return;
    }
    key_down[scan & 0x1FF] = (uint8_t)!up;
    {
        uint8_t bit = 0; int s = scan & 0xFF;
        if (s == 0x36) bit = 0x01;
        else if (s == 0x2A) bit = 0x02;
        else if (s == 0x1D) bit = 0x04;
        else if (s == 0x38) bit = 0x08;
        if (bit) {
            int held = (bit == 0x04) ? (key_down[0x1D] | key_down[0x11D]) : (bit == 0x08) ? (key_down[0x38] | key_down[0x138]) : !up;
            uint8_t f = RB(0x40, 0x17); f = held ? (uint8_t)(f | bit) : (uint8_t)(f & ~bit); WB(0x40, 0x17, f);
        }
    }
    if (scan & 0x100) kq[kq_tail++ & 255] = 0xE0;
    kq[kq_tail++ & 255] = (uint8_t)((scan & 0x7F) | (up ? 0x80 : 0));
    {
        int s = scan & 0x7F;
        int modifier = s == 0x2A || s == 0x36 || s == 0x1D || s == 0x38 || s == 0x3A || s == 0x45 || s == 0x46;
        if (!up && !modifier) bios_kbuf[bk_tail++ & 63] = (uint16_t)((s << 8) | (ascii & 0xFF));
    }
}
void rt_release_keys(void) { for (int s = 0; s < 0x200; s++) if (key_down[s]) rt_key_event(s, 1, 0); }
static char cfg_game_dir[MAX_PATH]; static int cfg_fullscreen, cfg_scale = 3, cfg_controller = 1, cfg_checkpoints = 0;
static void ini_path(const char *file, char *out) { plat_exe_dir(out); size_t l = strlen(out); snprintf(out + l, MAX_PATH - l, "/%s", file); }
static char *ini_trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s); while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}
static int ini_parse(char *line, char **k, char **v) {
    char *s = line; if ((uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) s += 3;
    s = ini_trim(s);
    if (*s == '[') { char *c = strchr(s, ']'); if (!c) return 0; *c = 0; *k = ini_trim(s + 1); return 1; }
    char *eq = strchr(s, '='); if (*s == ';' || *s == '#' || !eq || eq == s) return 0;
    *eq = 0; *k = ini_trim(s);
    char *val = eq + 1;
    for (char *c = val; *c; c++) if ((*c == ';' || *c == '#') && (c == val || c[-1] == ' ' || c[-1] == '\t')) { *c = 0; break; }
    *v = ini_trim(val); return 2;
}
static int ini_read(const char *file, const char *sect, const char *key, char *out, size_t n) {
    char ini[MAX_PATH], line[512], *k, *v; ini_path(file, ini);
    FILE *f = fopen(ini, "r"); if (!f) return 0;
    int in = 1, found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        int t = ini_parse(line, &k, &v);
        if (t == 1) in = ieq(k, sect);
        else if (t == 2 && in && ieq(k, key)) { snprintf(out, n, "%s", v); found = 1; }
    }
    fclose(f); return found;
}
static int poll_on = 100, poll_off = 2000;
static int ini_get(const char *key, char *out, size_t n) { return ini_read("pop2.ini", "pop2", key, out, n); }
static int ini_int(const char *key, int def) { char v[64]; return ini_get(key, v, sizeof v) ? atoi(v) : def; }
static size_t ini_put(char *out, size_t n, size_t cap, const char *s) { size_t l = strlen(s); if (n + l < cap) memcpy(out + n, s, l + 1); return n + l; }
static int ini_write(const char *file, const char *sect, const char *key, const char *value) {
    char ini[MAX_PATH], line[512], cut[512], kv[200], out[8192] = "", *k, *v; size_t n = 0; int in = 0, seen = 0, done = 0, nl = 1;
    ini_path(file, ini); snprintf(kv, sizeof kv, "%s=%s\n", key, value);
    FILE *f = fopen(ini, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            memcpy(cut, line, sizeof cut); int t = ini_parse(cut, &k, &v);
            if (t == 1) { if (in && !done) { n = ini_put(out, n, sizeof out, kv); done = 1; } in = ieq(k, sect); seen |= in; }
            else if (t == 2 && in && !done && ieq(k, key)) { n = ini_put(out, n, sizeof out, kv); done = 1; continue; }
            n = ini_put(out, n, sizeof out, line); nl = line[strlen(line) - 1] == '\n';
        }
        fclose(f);
    }
    if (!done) {
        char h[64]; snprintf(h, sizeof h, "[%s]\n", sect);
        if (!nl) n = ini_put(out, n, sizeof out, "\n");
        if (!seen) n = ini_put(out, n, sizeof out, h);
        n = ini_put(out, n, sizeof out, kv);
    }
    if (n >= sizeof out || !(f = fopen(ini, "w"))) { logmsg("settings: %s=%s not written to %s (%s)\n", key, value, ini, n >= sizeof out ? "file too long" : "cannot open"); return 0; }
    fputs(out, f); fclose(f); logmsg("settings: %s=%s written to %s\n", key, value, ini); return 1;
}
static void ini_set(const char *key, int value) { char v[16]; snprintf(v, sizeof v, "%d", value); ini_write("pop2.ini", "pop2", key, v); }
static void load_settings(void) {
    ini_get("GameDir", cfg_game_dir, MAX_PATH);
    cfg_fullscreen = ini_int("Fullscreen", 0);
    cfg_scale = ini_int("Scale", 3);
    if (cfg_scale < 1) cfg_scale = 1;
    if (cfg_scale > 8) cfg_scale = 8;
    cfg_controller = ini_int("Controller", 1);
    cfg_checkpoints = ini_int("Checkpoints", 0);
    { char m[16] = "port"; ini_get("Sound", m, sizeof m); cfg_sound_keep = ieq(m, "keep"); }
    cfg_filter = ini_int("Filter", 1); if (cfg_filter < 0 || cfg_filter >= plat_filter_modes()) cfg_filter = 1;
    cfg_aspect = ini_int("Aspect", 0);
    if (cfg_aspect < 0 || cfg_aspect >= plat_aspect_modes()) cfg_aspect = 0;
    logmsg("settings: Filter=%d (%s) Aspect=%d (%s)\n", cfg_filter, plat_filter_name(cfg_filter), cfg_aspect, plat_aspect_name(cfg_aspect));
}

#define DG 0x31F3
static uint8_t *cp_mem; static uint8_t cp_para[0x10000];
static Reg cp_ra, cp_rb, cp_rc, cp_rd; static uint16_t cp_si, cp_di, cp_bp, cp_sp, cp_ds, cp_es, cp_ss, cp_flags;
static int cp_room = -1, cp_level = -1;
int digi_busy(void); void digi_abort(void); void fm_all_notes_off(void);
static void cp_save_ext(void); static void cp_restore_ext(void);
void rt_frame_hook(void) {
    if (getenv("POP2_FPSLOG")) {
        static uint32_t lt; static int n; n++;
        if (plat_ms() - lt >= 1000) { logmsg("FPS %d\n", n); n = 0; lt = plat_ms(); }
    }
    if (getenv("POP2_SPLOG")) {
        static uint32_t lt; static uint16_t lo = 0xFFFF;
        if (SP < lo) lo = SP;
        if (plat_ms() - lt >= 1000) { logmsg("SS:SP %04x:%04x (lowest %04x)\n", SS, SP, lo); lt = plat_ms(); }
    }
    if (!cfg_checkpoints) return;
    int level = (int8_t)MEM[LIN(DG, 0x0A18)], room = MEM[LIN(DG, 0x5C0E)], hp = MEM[LIN(DG, 0x5C12)];
    int action = MEM[LIN(DG, 0x5C0B)];
    if (level != cp_level) { cp_level = level; cp_valid = 0; cp_room = -1; }
    if (cp_restore_pending) {
        cp_restore_pending = 0;
        if (cp_valid) {
            uint8_t tmin[2] = { MEM[LIN(DG, 0x5E2A)], MEM[LIN(DG, 0x5E2B)] }, ttick[2] = { MEM[LIN(DG, 0x5E44)], MEM[LIN(DG, 0x5E45)] };
            memcpy(MEM, cp_mem, 0x110000);
            memcpy(para_ovl, cp_para, sizeof cp_para);
            cp_restore_ext();
            MEM[LIN(DG, 0x5E2A)] = tmin[0]; MEM[LIN(DG, 0x5E2B)] = tmin[1];
            MEM[LIN(DG, 0x5E44)] = ttick[0]; MEM[LIN(DG, 0x5E45)] = ttick[1];
            rA = cp_ra; rB = cp_rb; rC = cp_rc; rD = cp_rd; SI = cp_si; DI = cp_di; BP = cp_bp; SP = cp_sp;
            DS = cp_ds; ES = cp_es; SS = cp_ss; set_flags(cp_flags);
            kq_head = kq_tail; bk_head = bk_tail;
            for (int s = 0; s < 0x59; s++) MEM[LIN(DG, 0x1E7B + s)] = (uint8_t)(key_down[s] | key_down[0x100 + s]);
            uint8_t f = 0;
            if (key_down[0x36]) f |= 1;
            if (key_down[0x2A]) f |= 2;
            if (key_down[0x1D] | key_down[0x11D]) f |= 4;
            if (key_down[0x38] | key_down[0x138]) f |= 8;
            WB(0x40, 0x17, f);
            digi_abort(); fm_all_notes_off();
            cp_kid_dead = 0;
            logmsg("checkpoint: restored room %d\n", cp_room);
            return;
        }
    }
    if (hp == 0) { if (!cp_kid_dead) { cp_kid_dead = 1; cp_dead_since = plat_ms(); } return; }
    cp_kid_dead = 0;
    if (room != 0 && room != cp_room && (action == 0 || action == 1 || action == 7) && !digi_busy()) {
        if (!cp_mem) cp_mem = malloc(0x110000);
        memcpy(cp_mem, MEM, 0x110000);
        memcpy(cp_para, para_ovl, sizeof cp_para);
        cp_save_ext();
        cp_ra = rA; cp_rb = rB; cp_rc = rC; cp_rd = rD; cp_si = SI; cp_di = DI; cp_bp = BP; cp_sp = SP;
        cp_ds = DS; cp_es = ES; cp_ss = SS; cp_flags = get_flags();
        cp_room = room; cp_valid = 1;
        logmsg("checkpoint: saved room %d (level %d)\n", room, level);
    }
}

void rt_savemenu_hook(void) {
    uint32_t now = plat_ms();
    logmsg("SAVE MENU opened (flag 1504=%d, level=%d)\n", RW(DG, 0x1504), (int8_t)MEM[LIN(DG, 0x0A18)]);
    for (int i = 0; i < 32; i++) {
        int k = (key_log_n + i) & 31;
        if (key_log[k].t) logmsg("   key  -%5u ms  scan %03x %s ascii %02x\n", now - key_log[k].t,
                                 key_log[k].scan, key_log[k].up ? "up  " : "down", key_log[k].ascii);
    }
    for (int i = 0; i < 32; i++) {
        int k = (doskey_log_n + i) & 31;
        if (doskey_log[k].t) logmsg("   read -%5u ms  key %04x\n", now - doskey_log[k].t, doskey_log[k].val);
    }
}

static uint64_t t_last_tick, t_last_present;
static void seq_key(int scan, int ascii) { rt_key_event(scan, 0, ascii); rt_key_event(scan, 1, 0); }
static void seq_text(const char *s) {
    static const char keys[] = "\0\033" "1234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./";
    for (; *s; s++) { int c = (*s >= 'A' && *s <= 'Z') ? *s + 32 : *s;
        for (int i = 1; i < (int)sizeof keys - 1; i++) if (keys[i] == c) { seq_key(i, *s); break; } }
}
static int menu_mode, menu_slot;
static uint32_t cutscene_seen;
void rt_cutscene_poll(void) { static uint32_t lt; uint32_t now = plat_ms(); if (now - lt > 2000) { lt = now; logmsg("cutscene: polling\n"); } cutscene_seen = now; }
void rt_copyprot_ran(void) { WW(DS, 0x0366, 1); }
void rt_dissolve_row(void) {
    uint32_t n = CX ? CX : 0x10000;
    if (n < 2) return;
    n--;
    uint16_t dx = DX, si = SI, di = DI; uint8_t bh = BH, bl = BL, al = AL, ah = AH;
    uint16_t step = FD ? 0xFFFF : 1;
    for (uint32_t i = 0; i < n; i++) {
        if (bh == 0) al = (uint8_t)dx;
        else if (bh == 1) al = (uint8_t)(dx >> 8);
        else { dx = (uint16_t)(dx * 5 + 1); ah = (uint8_t)(dx >> 8); al = (uint8_t)dx; bh = 0; }
        bh++;
        if (al > bl) { si++; di++; }
        else { WB(ES, di, RB(DS, si)); si += step; di += step; }
    }
    DX = dx; SI = si; DI = di; BH = bh; AL = al; AH = ah; CX = 1;
    poll_ctr -= (int)n;
}
void rt_menu_save_enter(void) { menu_mode = 1; menu_slot = 0; logmsg("menu: save screen\n"); }
void rt_menu_load_enter(void) { menu_mode = 2; menu_slot = 0; logmsg("menu: resume screen\n"); }
static void game_file(const char *name, char *out);
static void slot_name(int slot, char *out, int n) {
    char path[MAX_PATH]; out[0] = 0;
    game_file("PRINCE.SAV", path);
    FILE *f = fopen(path, "rb"); if (!f) return;
    char buf[25] = {0};
    if (fseek(f, 2 + 25L * slot, SEEK_SET) == 0 && fread(buf, 1, 24, f) > 0) snprintf(out, (size_t)n, "%s", buf);
    fclose(f);
}
void rt_menu_exit(void) { if (menu_mode) logmsg("menu: closed\n"); menu_mode = 0; }
enum { PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_CROSS, PAD_CIRCLE, PAD_SQUARE, PAD_TRI, PAD_L, PAD_R, PAD_START, PAD_SELECT,
       PAD_LS_UP, PAD_LS_DOWN, PAD_LS_LEFT, PAD_LS_RIGHT, PAD_RS_UP, PAD_RS_DOWN, PAD_RS_LEFT, PAD_RS_RIGHT, PAD_N, PAD_NONE = 0xFF };
static const char *const pad_names[PAD_N] = { "Up", "Down", "Left", "Right", "Cross", "Circle", "Square", "Triangle", "L", "R",
                                              "Start", "Select", "LStickUp", "LStickDown", "LStickLeft", "LStickRight",
                                              "RStickUp", "RStickDown", "RStickLeft", "RStickRight" };
static uint32_t pad_bits(const PlatPad *p) {
    const int on[PAD_N] = { p->up, p->down, p->left, p->right, p->b_cross, p->b_circle, p->b_square, p->b_tri, p->b_l, p->b_r,
                            p->b_start, p->b_select, p->ls_up, p->ls_down, p->ls_left, p->ls_right,
                            p->rs_up, p->rs_down, p->rs_left, p->rs_right };
    uint32_t b = 0; for (int i = 0; i < PAD_N; i++) if (on[i]) b |= 1u << i;
    return b;
}
enum { ACT_KEY, ACT_TAP, ACT_VIEW, ACT_FILTER,
       ACT_NEXT, ACT_OK, ACT_CANCEL };
static const struct { const char *name, *def, *desc; int kind, scan, ascii, need; } pad_acts[] = {
    { "Jump",         "Up, LStickUp, Cross", "jump, climb, block",        ACT_KEY, 0x148, 0,  2 },
    { "Crouch",       "Down, LStickDown", "crouch, put the sword away",   ACT_KEY, 0x150, 0,  2 },
    { "Left",         "Left, LStickLeft", "run left",                     ACT_KEY, 0x14B, 0,  2 },
    { "Right",        "Right, LStickRight", "run right",                  ACT_KEY, 0x14D, 0,  2 },
    { "Action",       "Circle",     "grab, hang on, step carefully",      ACT_KEY, 0x2A,  0,  2 },
    { "Strike",       "Square",     "draw the sword, strike",             ACT_KEY, 0x1D,  0,  2 },
    { "Pause",        "Start",      "pause",                              ACT_KEY, 0x01,  27, 1 },
    { "Time",         "Select",     "show the time left",                 ACT_KEY, 0x39,  32, 0 },
    { "SaveScreen",   "L",          "the game's save screen",             ACT_TAP, 0x22,  0,  0 },
    { "ResumeScreen", "R",          "the game's resume screen",           ACT_TAP, 0x26,  0,  0 },
    { "Restart",      "Select+L",   "restart the level",                  ACT_TAP, 0x1E,  0,  0 },
    { "View",         "Triangle+L", "the next view: 4:3, Wide",           ACT_VIEW, 0,    0,  0 },
    { "Filter",       "Triangle+R", "the next filter: Sharp, Pixel, Soft, LCD", ACT_FILTER, 0, 0, 0 },
    { "ScreenNext",   "Down, LStickDown", "save and resume screens: the next slot", ACT_NEXT, 0, 0, 1 },
    { "ScreenConfirm", "Cross",     "save and resume screens: save (name it) or resume", ACT_OK, 0, 0, 1 },
    { "ScreenCancel", "Circle, Start", "save and resume screens: leave",  ACT_CANCEL, 0, 0, 1 },
};
static int pad_screen(int a) { return pad_acts[a].kind >= ACT_NEXT; }
enum { PAD_ACTS = sizeof pad_acts / sizeof pad_acts[0], PAD_ALTS = 4 };
static struct { int n; uint8_t mod[PAD_ALTS], btn[PAD_ALTS]; } pad_bind[PAD_ACTS];
static const char pad_file[] = "controls.ini";
static char pad_notes[8][80]; static int pad_note_n;
static void pad_note(const char *fmt, ...) {
    char t[80]; va_list ap; va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    logmsg("controls: %s\n", t);
    if (pad_note_n < 8) snprintf(pad_notes[pad_note_n++], sizeof pad_notes[0], "%s", t);
}
static int pad_name(const char *s) { for (int i = 0; i < PAD_N; i++) if (ieq(s, pad_names[i])) return i; return -1; }
static int pad_parse(int a, const char *value, char *bad, size_t nb) {
    char buf[160], *s = buf; snprintf(buf, sizeof buf, "%s", value); pad_bind[a].n = 0;
    char *cmt = strpbrk(buf, ";#"); if (cmt) *cmt = 0;
    for (;;) {
        char *c = strchr(s, ','); if (c) *c = 0;
        char *tok = ini_trim(s), *plus = strchr(tok, '+'); int m = PAD_NONE, b = -1;
        if (*tok && !ieq(tok, "None")) {
            if (plus) {
                *plus = 0; char *t1 = ini_trim(tok), *t2 = ini_trim(plus + 1);
                if (!*t1 || !*t2 || strchr(t2, '+')) { snprintf(bad, nb, "a chord is two buttons"); return 0; }
                if ((m = pad_name(t1)) < 0 || (b = pad_name(t2)) < 0) { snprintf(bad, nb, "%s is no button", m < 0 ? t1 : t2); return 0; }
                if (m == b) { snprintf(bad, nb, "%s+%s is one button twice", t1, t2); return 0; }
            } else if ((b = pad_name(tok)) < 0) { snprintf(bad, nb, "%s is no button", tok); return 0; }
            if (pad_bind[a].n == PAD_ALTS) { snprintf(bad, nb, "more than %d buttons", PAD_ALTS); return 0; }
            pad_bind[a].mod[pad_bind[a].n] = (uint8_t)m; pad_bind[a].btn[pad_bind[a].n++] = (uint8_t)b;
        }
        if (!c) return 1;
        s = c + 1;
    }
}
static const char *pad_alt(int a, int i, char *out, size_t n) {
    int m = pad_bind[a].mod[i];
    snprintf(out, n, "%s%s%s", m == PAD_NONE ? "" : pad_names[m], m == PAD_NONE ? "" : "+", pad_names[pad_bind[a].btn[i]]);
    return out;
}
static void pad_line(int a, char *out, size_t n) {
    int w = 26 - (int)strlen(pad_acts[a].name) - 1 - (int)strlen(pad_acts[a].def); if (w < 1) w = 1;
    snprintf(out, n, "%s%*s; %s", pad_acts[a].def, w, "", pad_acts[a].desc);
}
static int controls_write(const char *path) {
    char v[160]; FILE *f = fopen(path, "w");
    if (!f) { logmsg("controls: %s cannot be written\n", path); return 0; }
    fputs("; Prince of Persia 2 - the controls. Read when the game starts: edit, save, start the game again.\n; Buttons:", f);
    for (int i = 0; i < PAD_N; i++) fprintf(f, "%s %s", i == PAD_LS_UP ? "\n;         " : "", pad_names[i]);
    fputs(" None\n"
          "; Several buttons: Up, Cross    A chord, the first held and the second pressed: Triangle+L\n"
          "; A button held for a chord still does its own action: keep Jump, Crouch, Left, Right, Action and Strike off it.\n"
          "; A mistake falls back to that action's default and is named on the screen at start.\n"
          "; Delete a line for its default; delete this file for every default.\n"
          "; The Screen actions work in the save and resume screens only, the others outside them.\n"
          "; Fixed: any button skips a cutscene.\n"
          "[controls]\n", f);
    for (int a = 0; a < PAD_ACTS; a++) { pad_line(a, v, sizeof v); fprintf(f, "%s=%s\n", pad_acts[a].name, v); }
    fclose(f); logmsg("controls: %s written with every default\n", path);
    return 1;
}
static void controls_load(void) {
    static char val[PAD_ACTS][160]; int got[PAD_ACTS], n_got = 0, missing = 0, last = 0;
    char path[MAX_PATH], bad[80], x[32], y[32]; ini_path(pad_file, path);
    if (!plat_file_exists(path)) controls_write(path);
    for (int a = 0; a < PAD_ACTS; a++) n_got += got[a] = ini_read(pad_file, "controls", pad_acts[a].name, val[a], sizeof val[a]);
    if (!n_got) {
        FILE *f = fopen(path, "rb"); long size = -1;
        if (f) { fseek(f, 0, SEEK_END); size = ftell(f); fclose(f); }
        int keep = size > 0; char aside[MAX_PATH]; ini_path("controls-unreadable.ini", aside);
        if (keep) remove(aside);
        if (keep && rename(path, aside) != 0) pad_note("no action readable - defaults used");
        else if (!controls_write(path)) pad_note("controls.ini cannot be written - defaults used");
        else pad_note(keep ? "no action readable - kept as controls-unreadable.ini" : "controls.ini was empty - every default written");
    } else for (int a = 0; a < PAD_ACTS; a++) if (!got[a]) {
        char line[160]; pad_line(a, line, sizeof line); ini_write(pad_file, "controls", pad_acts[a].name, line);
        missing++; last = a;
    }
    if (missing == 1) pad_note("%s was missing - default added", pad_acts[last].name);
    else if (missing) pad_note("%d actions were missing - defaults added", missing);
    for (int a = 0; a < PAD_ACTS; a++) {
        const char *v = got[a] ? val[a] : pad_acts[a].def;
        if (!pad_parse(a, v, bad, sizeof bad)) { pad_note("%s: %s - default %s used", pad_acts[a].name, bad, pad_acts[a].def); pad_parse(a, pad_acts[a].def, bad, sizeof bad); }
        else if (!pad_bind[a].n && pad_acts[a].need) { pad_note("%s has no button - default %s used", pad_acts[a].name, pad_acts[a].def); pad_parse(a, pad_acts[a].def, bad, sizeof bad); }
    }
    uint32_t warned[PAD_ACTS] = {0};
    for (int a = 0; a < PAD_ACTS; a++) for (int i = 0; i < pad_bind[a].n; i++) {
        int m = pad_bind[a].mod[i], b = pad_bind[a].btn[i];
        for (int a2 = a + 1; a2 < PAD_ACTS; a2++) for (int j = 0; j < pad_bind[a2].n; j++)
            if (pad_screen(a2) == pad_screen(a) && pad_bind[a2].mod[j] == m && pad_bind[a2].btn[j] == b) pad_note("%s does both %s and %s", pad_alt(a, i, x, sizeof x), pad_acts[a].name, pad_acts[a2].name);
        if (m == PAD_NONE || pad_screen(a)) continue;
        for (int a2 = 0; a2 < PAD_ACTS; a2++) if (pad_acts[a2].need == 2 && !(warned[a2] & 1u << m))
            for (int j = 0; j < pad_bind[a2].n; j++) if (pad_bind[a2].mod[j] == PAD_NONE && pad_bind[a2].btn[j] == m) {
                pad_note("%s is held for %s and also does %s", pad_names[m], pad_acts[a].name, pad_acts[a2].name); warned[a2] |= 1u << m; break;
            }
    }
    for (int a = 0; a < PAD_ACTS; a++) {
        char t[160] = ""; for (int i = 0; i < pad_bind[a].n; i++) { if (i) strcat(t, ", "); strcat(t, pad_alt(a, i, y, sizeof y)); }
        logmsg("controls: %s=%s\n", pad_acts[a].name, pad_bind[a].n ? t : "None");
    }
}
static void controls_osd(void) {
    static int shown; static uint32_t next;
    if (shown >= pad_note_n || !plat_ready || (shown && (int32_t)(plat_ms() - next) < 0)) return;
    char t[96]; snprintf(t, sizeof t, "controls.ini %d/%d: %s", shown + 1, pad_note_n, pad_notes[shown]);
    plat_osd(t, 4000); next = plat_ms() + 4000; shown++;
}
static uint32_t pad_chorded(int screen, uint32_t on) {
    uint32_t c = 0;
    for (int a = 0; a < PAD_ACTS; a++) if (pad_screen(a) == screen) for (int i = 0; i < pad_bind[a].n; i++)
        if (pad_bind[a].mod[i] != PAD_NONE && (on & 1u << pad_bind[a].mod[i])) c |= 1u << pad_bind[a].btn[i];
    return c;
}
static int pad_fire(int a, uint32_t on, uint32_t land, uint32_t chorded, int *hit) {
    int h = 0, fire = 0;
    for (int i = 0; i < pad_bind[a].n; i++) {
        uint32_t b = 1u << pad_bind[a].btn[i], m = pad_bind[a].mod[i] == PAD_NONE ? 0 : 1u << pad_bind[a].mod[i];
        if (m ? (on & m) != 0 : !(chorded & b)) { h |= (on & b) != 0; fire |= (land & b) != 0; }
    }
    if (hit) *hit = h;
    return fire;
}
static void pad_poll(void) {
    static uint8_t held[0x200], any_was; static uint32_t was, menu_was; static int quiet;
    if (!cfg_controller) return;
    PlatPad p; memset(&p, 0, sizeof p);
    plat_pad_read(&p);
    uint32_t on = pad_bits(&p), land = on & ~was, menu_land = on & ~menu_was;
    menu_was = on;
    if (menu_mode || quiet) {
        for (int sc = 0; sc < 0x200; sc++) if (held[sc]) { held[sc] = 0; rt_key_event(sc, 1, 0); }
        uint32_t chorded = pad_chorded(1, on), used = 0;
        for (int a = 0; a < PAD_ACTS; a++) if (pad_screen(a)) {
            for (int i = 0; i < pad_bind[a].n; i++)
                used |= 1u << pad_bind[a].btn[i] | (pad_bind[a].mod[i] == PAD_NONE ? 0 : 1u << pad_bind[a].mod[i]);
            if (!menu_mode || !pad_fire(a, on, menu_land, chorded, NULL)) continue;
            if (pad_acts[a].kind == ACT_NEXT) { seq_key(0x0F, 9); menu_slot = (menu_slot + 1) % 10; }
            else if (pad_acts[a].kind == ACT_OK && menu_mode == 2) seq_key(0x1C, 13);
            else if (pad_acts[a].kind == ACT_OK) {
                char old[32], name[32]; slot_name(menu_slot, old, sizeof old);
                int len = plat_text_input("Name for the saved game", old, name, 21);
                t_last_tick = plat_us();
                if (len > 0 && strcmp(name, old) == 0) seq_key(0x1C, 13);
                else if (len > 0) {
                    for (size_t k = 0; k < strlen(old); k++) seq_key(0x153, 0);
                    seq_text(name); seq_key(0x1C, 13);
                }
                logmsg("menu: slot %d -> %s\n", menu_slot + 1, len <= 0 ? "(cancelled)" : strcmp(name, old) ? "renamed" : "kept");
            }
            else seq_key(0x01, 27);
            logmsg("menu: pad %s\n", pad_acts[a].name);
        }
        quiet = menu_mode || (on & used) != 0;
        was = ~0u;
        return;
    }
    if (cutscene_seen && plat_ms() - cutscene_seen < 250) {
        for (int sc = 0; sc < 0x200; sc++) if (held[sc]) { held[sc] = 0; rt_key_event(sc, 1, 0); }
        if (on && !any_was) { seq_key(0x39, 32); logmsg("pad: cutscene skip\n"); }
        any_was = (uint8_t)(on ? 1 : 0); was = on;
        return;
    }
    any_was = 1;
    uint32_t chorded = pad_chorded(0, on);
    for (int a = 0; a < PAD_ACTS; a++) if (!pad_screen(a)) {
        int hit, fire = pad_fire(a, on, land, chorded, &hit);
        switch (pad_acts[a].kind) {
        case ACT_KEY: { int sc = pad_acts[a].scan; if (hit != held[sc]) { held[sc] = (uint8_t)hit; rt_key_event(sc, !hit, pad_acts[a].ascii); } break; }
        case ACT_TAP: if (fire) { seq_key(pad_acts[a].scan, 0); logmsg("pad: %s\n", pad_acts[a].name); } break;
        case ACT_VIEW: if (fire) rt_cycle_aspect(); break;
        case ACT_FILTER: if (fire) rt_cycle_filter(); break;
        }
    }
    was = on;
}
static void pump(void) {
    if (plat_pump()) { logmsg("window closed\n"); exit(0); }
    pad_poll();
    controls_osd();
}

static uint32_t ivt_seg[256], ivt_off[256];
static double pit_hz = 1193182.0 / 65536.0;
static int pit_latch_lo, pit_write_hi; static uint16_t pit_reload = 0;
static int in_irq;

static void raise_irq(int vec) {
    uint16_t off = RW(0, vec * 4), seg = RW(0, vec * 4 + 2);
    uint16_t bp0 = BP, sp0 = SP, ax0 = AX, bx0 = BX, cx0 = CX, dx0 = DX, si0 = SI, di0 = DI, ds0 = DS, es0 = ES;
    PUSH(get_flags()); FI = 0;
    PUSH(0xFFFF); PUSH(0xFFFF);
    call_far(seg, off);
    {
        static int n;
        if (n < 20 && (BP != bp0 || SP != sp0 || AX != ax0 || BX != bx0 || CX != cx0 || DX != dx0
                       || SI != si0 || DI != di0 || DS != ds0 || ES != es0)) {
            n++;
            logmsg("IRQ %d handler %04x:%04x changed registers: bp %04x->%04x sp %04x->%04x ax %04x->%04x "
                   "bx %04x->%04x cx %04x->%04x dx %04x->%04x si %04x->%04x di %04x->%04x ds %04x->%04x es %04x->%04x (in %s)\n",
                   vec, seg, off, bp0, BP, sp0, SP, ax0, AX, bx0, BX, cx0, CX, dx0, DX, si0, SI, di0, DI, ds0, DS, es0, ES, cur_fn);
        }
    }
}
static long n_tick_checks, n_presents, n_irq8;
typedef struct { uint32_t t, hold; int scan; int state; } AutoKey;
static AutoKey autokeys[256]; static int n_autokeys; static uint32_t t_start;
static void autokeys_init(void) {
    const char *s = getenv("POP2_KEYS"); t_start = plat_ms();
    while (s && *s && n_autokeys < 256) {
        unsigned t = 0, sc = 0, hold = 80; int n = 0;
        if (sscanf(s, "%u:%x:%u%n", &t, &sc, &hold, &n) == 3 || (hold = 80, sscanf(s, "%u:%x%n", &t, &sc, &n) == 2)) {
            autokeys[n_autokeys].t = t; autokeys[n_autokeys].scan = (int)sc; autokeys[n_autokeys].hold = hold; n_autokeys++;
        }
        s = strchr(s, ','); if (s) s++;
    }
}
static void autokeys_poll(void) {
    uint32_t now = plat_ms() - t_start;
    for (int i = 0; i < n_autokeys; i++) {
        AutoKey *k = &autokeys[i];
        if (k->state == 0 && now >= k->t) {
            int asc = plat_key_ascii(k->scan, key_down[0x2A] || key_down[0x36], key_down[0x1D] || key_down[0x11D]);
            if (key_down[0x38] || key_down[0x138]) asc = 0;
            rt_key_event(k->scan, 0, asc); k->state = 1;
        }
        else if (k->state == 1 && now >= k->t + k->hold) { rt_key_event(k->scan, 1, 0); k->state = 2; }
    }
}
static void tick_check(void) {
    n_tick_checks++;
    if (in_irq) return;
    { static int arm = -1; if (arm < 0) arm = getenv("POP2_DUMP_ALL") != NULL;
      if (arm && plat_ms() - t_start > 3000) dump_all_overlays(); }
    if (n_autokeys) autokeys_poll();
    static int timelog = -1, cplog = -1;
    if (timelog < 0) { timelog = getenv("POP2_TIMELOG") != NULL; cplog = getenv("POP2_CPLOG") != NULL; }
    if (timelog) {
        static uint32_t lt; uint32_t now = plat_ms();
        if (now - lt > 1000) { lt = now; logmsg("TIME real=%ums game=%d min + %d ticks (flag 01d6=%d, kid alive 5c91=%d, hp=%d)\n",
            now - t_start, (int16_t)RW(0x31F3, 0x5E2A), (int16_t)RW(0x31F3, 0x5E44),
            (int8_t)MEM[LIN(0x31F3, 0x01D6)], (int8_t)MEM[LIN(0x31F3, 0x5C91)], MEM[LIN(0x31F3, 0x5C12)]);
            logmsg("     level a18=%d 6ccc=%d 5e00=%d\n", (int8_t)MEM[LIN(0x31F3, 0x0A18)],
                   (int8_t)MEM[LIN(0x31F3, 0x6CCC)], (int16_t)RW(0x31F3, 0x5E00)); }
    }
    if (cplog) {
        static uint32_t lt; uint32_t now = plat_ms();
        if (now - lt > 1000) { lt = now; logmsg("CP t=%lu level=%d room=%d cp_room=%d start_room=%d hp=%d\n", now - t_start,
            (int8_t)MEM[LIN(0x31F3, 0x0A18)], MEM[LIN(0x31F3, 0x5C0E)], MEM[LIN(0x31F3, 0x6CCD)], MEM[LIN(0x31F3, 0x5E34)], MEM[LIN(0x31F3, 0x5C12)]); }
    }
    if (FI) { extern void snd_poll(void); in_irq = 1; snd_poll(); in_irq = 0; }
    uint64_t now = plat_us();
    double dt = (double)(int64_t)(now - t_last_tick) / 1e6;
    double period = 1.0 / pit_hz;
    int n = 0;
    if (FI) {
        while (dt >= period && n < 4) {
            t_last_tick += (uint64_t)(period * 1e6);
            dt -= period; n++;
            in_irq = 1; n_irq8++;
            raise_irq(8);
            in_irq = 0;
        }
        if (dt > 0.25) {
            t_last_tick = plat_us();
        }
        if (kq_head != kq_tail && !in_irq) {
            port60 = kq[kq_head++ & 255];
            in_irq = 1; raise_irq(9); in_irq = 0;
        }
    } else if (dt > 0.5) t_last_tick = plat_us();
    double pd = (double)(int64_t)(now - t_last_present) / 1e6;
    if (pd > 1.0 / 70) { pump(); if (video_mode == 0x13) { present(); n_presents++; } t_last_present = plat_us(); }
}

int poll_ctr = 2000;
const char *volatile cur_fn = "start";
static int watchdog(void *p) {
    (void)p;
    for (;;) { plat_sleep_ms(1000); logmsg("[watch] ticks %ld irq8 %ld presents %ld in_irq %d mode %x\n", n_tick_checks, n_irq8, n_presents, in_irq, video_mode); logmsg("[watch] in %s  ax=%04x bx=%04x cx=%04x dx=%04x ds=%04x es=%04x ss:sp=%04x:%04x\n",
               cur_fn, AX, BX, CX, DX, DS, ES, SS, SP);
        if (getenv("POP2_WATCHSTACK")) {
            for (int i = 0; i < 28; i += 2)
                logmsg("[watch]   sp+%02x: %04x %04x\n", i * 2, RW(SS, (uint16_t)(SP + i * 2)), RW(SS, (uint16_t)(SP + i * 2 + 2)));
        } }
    return 0;
}
static const char *ring[16]; static uint16_t ring_sp[16], ring_bp[16]; static unsigned n_ring;
void rt_fatal_hook(void) {
    logmsg("GAME FATAL EXIT: ss:sp %04x:%04x bp %04x ds %04x - args:", SS, SP, BP, DS);
    for (int i = 0; i < 6; i++) logmsg(" %04x", RW(SS, (uint16_t)(SP + i * 2)));
    logmsg("\n");
    for (int i = 0; i < 16; i++) { const char *f = ring[(n_ring + i) & 15]; if (f) logmsg("      %s\n", f); }
}

#define WAIT_FN "f_root_1f74_0134"
static int tick_sleep(void) {
    int64_t left = (int64_t)(t_last_tick + (uint64_t)(1e6 / pit_hz)) - (int64_t)plat_us();
    if (left <= 700) return 0;
    plat_sleep_us((int)(left - 500)); return 1;
}
static void wait_poll(void) {
    if (lever_on(LEVER_SLEEP)) tick_sleep();
}
void rt_poll_at(const char *fn) {
    static const char *wait_fn;
    if (fn == wait_fn || (!wait_fn && !strcmp(fn, WAIT_FN) && (wait_fn = fn))) { wait_poll(); in_wait = 1; rt_poll(); in_wait = 0; }
    else rt_poll();
}
void rt_poll(void) {
    poll_ctr = lever_on(LEVER_POLL) ? poll_on : poll_off;
    static int splog = -1; if (splog < 0) splog = getenv("POP2_SPLOG") != NULL;
    if (splog) {
        static uint16_t low = 0xFFFF; static int n;
        static uint16_t prev; static int init;
        if (!init) { init = 1; prev = SP; }
        if ((uint16_t)(SP - prev) > 0x400 && (uint16_t)(prev - SP) > 0x400 && n < 60) {
            n++; logmsg("SP JUMP %04x -> %04x (%+d) in %s\n", prev, SP, (int16_t)(SP - prev), cur_fn);
        }
        prev = SP;
        if (SP < low - 0x200 && n < 60) { low = SP; n++; logmsg("SP falling: %04x:%04x in %s\n", SS, SP, cur_fn); }
    }
    tick_check();
}

static int vsync_phase;
static uint32_t pit2_reload = 0x10000; static uint64_t pit2_t0; static int pit2_wr_hi, pit2_rd_hi, pit2_latched; static uint16_t pit2_latch;
static uint16_t pit2_count(void) {
    uint64_t ticks = (uint64_t)((double)(plat_us() - pit2_t0) * 1193182.0 / 1e6);
    return (uint16_t)(pit2_reload - ticks % pit2_reload);
}
uint8_t port_in8(uint16_t port) {
    tick_check();
    switch (port) {
    case 0x60: return port60;
    case 0x61: return 0;
    case 0x64: return (kq_head != kq_tail) ? 1 : 0;
    case 0x3DA: {
        vsync_phase = (vsync_phase + 1) & 7;
        if (vsync_phase == 0) {
            plat_sleep_ms(0);
        }
        return (vsync_phase < 2) ? 0x09 : 0x00;
    }
    case 0x3C9: { uint8_t v = pal[pal_read_idx][pal_read_comp]; if (++pal_read_comp == 3) { pal_read_comp = 0; pal_read_idx = (pal_read_idx + 1) & 255; } return v; }
    case 0x40: return 0;
    case 0x42: {
        uint16_t c = pit2_latched ? pit2_latch : pit2_count();
        uint8_t v = pit2_rd_hi ? (uint8_t)(c >> 8) : (uint8_t)c;
        if (pit2_rd_hi) pit2_latched = 0;
        pit2_rd_hi ^= 1;
        return v; }
    case 0x21: return 0;
    case 0x201: return 0xFF;
    case 0x388: case 0x389: return fm_port_read();
    }
    return 0xFF;
}
uint16_t port_in16(uint16_t port) { return (uint16_t)(port_in8(port) | (port_in8((uint16_t)(port + 1)) << 8)); }
void port_out8(uint16_t port, uint8_t v) {
    switch (port) {
    case 0x3C8: pal_idx = v; pal_comp = 0; return;
    case 0x3C7: pal_read_idx = v; pal_read_comp = 0; return;
    case 0x3C9:
        pal[pal_idx][pal_comp] = v & 63; if (++pal_comp == 3) { pal_comp = 0; pal_idx = (pal_idx + 1) & 255; } return;
    case 0x43:
        if ((v >> 6) == 2) {
            if (((v >> 4) & 3) == 0) { pit2_latch = pit2_count(); pit2_latched = 1; pit2_rd_hi = 0; }
            else { pit2_wr_hi = 0; pit2_rd_hi = 0; }
            return;
        }
        pit_write_hi = 0; return;
    case 0x42:
        if (!pit2_wr_hi) { pit2_reload = (pit2_reload & 0xFF00) | v; pit2_wr_hi = 1; }
        else { pit2_reload = (pit2_reload & 0xFF) | ((uint32_t)v << 8); if (!pit2_reload) pit2_reload = 0x10000; pit2_wr_hi = 0; pit2_t0 = plat_us(); }
        return;
    case 0x40:
        if (!pit_write_hi) { pit_latch_lo = v; pit_write_hi = 1; }
        else { pit_reload = (uint16_t)(pit_latch_lo | (v << 8)); pit_write_hi = 0;
               pit_hz = 1193182.0 / (pit_reload ? pit_reload : 65536); logmsg("PIT rate %.1f Hz\n", pit_hz); }
        return;
    case 0x20: case 0x21: case 0x61: return;
    case 0x388: case 0x389: fm_port_write(port, v); return;
    }
}
void port_out16(uint16_t port, uint16_t v) { port_out8(port, (uint8_t)v); port_out8((uint16_t)(port + 1), (uint8_t)(v >> 8)); }

static char game_dir[MAX_PATH];
typedef struct { char name[260]; uint8_t *buf; uint32_t size; int refs, stale; } CachedFile;
static CachedFile cached[32];
typedef struct { FILE *f; CachedFile *c; uint32_t pos; char name[260]; } DosFile;
static DosFile files[64];
#define DOS_OPEN(i) (files[i].f || files[i].c)
static CachedFile *cache_get(const char *path) {
    CachedFile *slot = NULL;
    for (int i = 0; i < 32; i++) {
        if (cached[i].buf && !cached[i].stale && !strcmp(cached[i].name, path)) return &cached[i];
        if (!cached[i].buf && !slot) slot = &cached[i];
    }
    if (!slot) return NULL;
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    long sz = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    uint8_t *b = (sz >= 0 && sz <= 16L * 1024 * 1024) ? malloc(sz ? (size_t)sz : 1) : NULL;
    if (!b || fseek(f, 0, SEEK_SET) != 0 || fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f);
    strncpy(slot->name, path, 259); slot->name[259] = 0; slot->buf = b; slot->size = (uint32_t)sz; slot->refs = 0; slot->stale = 0;
    logmsg("cache: %s, %u bytes\n", path, slot->size);
    return slot;
}
static void cache_release(CachedFile *c) { if (c && --c->refs <= 0 && c->stale) { free(c->buf); memset(c, 0, sizeof *c); } }
static void cache_drop(const char *path) {
    for (int i = 0; i < 32; i++) if (cached[i].buf && !strcmp(cached[i].name, path)) {
        if (cached[i].refs > 0) cached[i].stale = 1; else { free(cached[i].buf); memset(&cached[i], 0, sizeof cached[i]); }
    }
}
static void dos_close(int h) { if (files[h].f) fclose(files[h].f); cache_release(files[h].c); files[h].f = NULL; files[h].c = NULL; files[h].pos = 0; }
static void game_file(const char *name, char *out) {
    char n[260]; snprintf(n, sizeof n, "%s", name);
    if (plat_case_fold()) for (char *c = n; *c; c++) *c = (char)tolower((unsigned char)*c);
    PATH_FMT(out, "%s/%s", game_dir, n);
}
static char file_path[32][MAX_PATH];
static const char *checked_file(const char *name) {
    for (int i = 0; pop2_files[i].name && i < 32; i++) if (ieq(pop2_files[i].name, name) && file_path[i][0]) return file_path[i];
    return NULL;
}
static int file_sha256(const char *path, uint8_t out[32]) {
    static uint8_t buf[65536];
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    Sha256 s; sha256_init(&s);
    size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) sha256_update(&s, buf, n);
    int ok = !ferror(f); fclose(f);
    sha256_final(&s, out);
    return ok;
}
static void name_list(char *list, size_t cap, int *count, const char *name) {
    size_t l = strlen(list);
    if (++*count <= 6) snprintf(list + l, cap - l, "%s%s", l ? ", " : "", name);
}
static void check_files(void) {
    int n = 0; while (pop2_files[n].name) n++;
    if (!n) return;
    if (n > 32) fatal("versions/%s.py FILES: %d files, file_path holds 32", pop2_version, n);
    char stamp[MAX_PATH]; ini_path("pop2_files.txt", stamp);
    char *old = NULL;
    FILE *sf = fopen(stamp, "rb");
    if (sf) {
        long sz = fseek(sf, 0, SEEK_END) == 0 ? ftell(sf) : -1;
        if (sz > 0 && sz < 65536 && (old = malloc((size_t)sz + 1)) && fseek(sf, 0, SEEK_SET) == 0 && fread(old, 1, (size_t)sz, sf) == (size_t)sz) old[sz] = 0;
        else { free(old); old = NULL; }
        fclose(sf);
    }
    static char lines[32 * (MAX_PATH + 100)]; char missing[200] = "", modified[200] = ""; lines[0] = 0;
    int n_missing = 0, n_modified = 0, hashed = 0; long bytes = 0; const char *other = NULL;
    uint64_t t0 = plat_us();
    for (int i = 0; i < n; i++) {
        const GameFile *g = &pop2_files[i];
        const char *cand[3] = {g->name, g->alt[0], g->alt[1]};
        char hex[65]; for (int k = 0; k < 32; k++) snprintf(hex + 2 * k, 3, "%02x", g->sha256[k]);
        int seen = 0;
        file_path[i][0] = 0;
        for (int c = 0; c < 3 && cand[c] && !file_path[i][0]; c++) {
            char p[MAX_PATH], line[MAX_PATH + 100]; struct stat st;
            game_file(cand[c], p);
            if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            seen = 1;
            int exe = ieq(g->name, "PRINCE.EXE");
            if ((uint32_t)st.st_size != g->size && !exe) continue;
            snprintf(line, sizeof line, "%s %ld %lld %s\n", hex, (long)st.st_size, (long long)st.st_mtime, p);
            if (old && strstr(old, line)) { strcpy(file_path[i], p); }
            else {
                uint8_t got[32];
                if (!file_sha256(p, got)) continue;
                hashed++; bytes += (long)st.st_size;
                if (!memcmp(got, g->sha256, 32)) strcpy(file_path[i], p);
                else if (exe) for (const OtherExe *o = pop2_other_exes; o->what; o++) if (!memcmp(got, o->sha256, 4)) other = o->what;
            }
            if (file_path[i][0]) strcat(lines, line);
        }
        if (!file_path[i][0]) name_list(seen ? modified : missing, sizeof missing, seen ? &n_modified : &n_missing, g->name);
    }
    free(old);
    logmsg("files: %d of %d passed, %d hashed (%ld bytes, %u ms)\n", n - n_missing - n_modified, n, hashed, bytes, (unsigned)((plat_us() - t0) / 1000));
    if (hashed && (sf = fopen(stamp, "wb"))) { fputs(lines, sf); fclose(sf); }
    if (other)
        fatal("PRINCE.EXE is %s. Use the original v1.1; the manual question is skipped.", other);
    if (n_missing || n_modified) {
        char msg[600]; int l = snprintf(msg, sizeof msg, "v1.1 game files %s", n_missing ? "incomplete. Missing: " : "modified: ");
        if (n_missing) l += snprintf(msg + l, sizeof msg - (size_t)l, "%s", missing);
        if (n_missing > 6) l += snprintf(msg + l, sizeof msg - (size_t)l, " +%d more", n_missing - 6);
        if (n_modified) l += snprintf(msg + l, sizeof msg - (size_t)l, "%s%s", n_missing ? ". Modified: " : "", modified);
        if (n_modified > 6) snprintf(msg + l, sizeof msg - (size_t)l, " +%d more", n_modified - 6);
        fatal("%s", msg);
    }
}
static const char *sound_sub(const char *name) {
    static char cfg[MAX_PATH]; static int cfg_done;
    if (cfg_sound_keep) return NULL;
    if (ieq(name, "MIDI.DRV")) return checked_file("msb_pro.drv") ? checked_file("msb_pro.drv") : "msb_pro.drv";
    if (ieq(name, "DIGI.DRV")) return checked_file("dsb_pro.drv") ? checked_file("dsb_pro.drv") : "dsb_pro.drv";
    if (ieq(name, "PRESETS.DEF")) return checked_file("presets.def");
    if (!ieq(name, "CONFIG.DAT")) return NULL;
    if (!cfg_done) {
        cfg_done = 1;
        char src[MAX_PATH]; game_file("CONFIG.DAT", src);
        uint8_t b[64]; size_t n = 0;
        FILE *f = fopen(src, "rb");
        if (f) { n = fread(b, 1, sizeof b, f); fclose(f); }
        if (n == 32) {
            b[4] = 1; b[5] = 0;
            b[6] = 1; b[7] = 0;
            b[8] = 0x21; b[9] = 0;
            char exe[MAX_PATH]; plat_exe_dir(exe);
            PATH_FMT(cfg, "%s/pop2_config.dat", exe);
            FILE *g = fopen(cfg, "wb");
            if (!g) {
                char tmp[MAX_PATH]; plat_temp_dir(tmp);
                PATH_FMT(cfg, "%s/pop2_config.dat", tmp); g = fopen(cfg, "wb");
            }
            if (g) { fwrite(b, 1, n, g); fclose(g); } else cfg[0] = 0;
        }
    }
    return cfg[0] ? cfg : NULL;
}
static void host_path(uint16_t seg, uint16_t off, char *out) {
    char dos[260]; int i = 0;
    while (i < 255 && RB(seg, (uint16_t)(off + i))) { dos[i] = (char)RB(seg, (uint16_t)(off + i)); i++; }
    dos[i] = 0;
    const char *p = dos;
    if (p[0] && p[1] == ':') p += 2;
    const char *last = p;
    for (const char *q = p; *q; q++) if (*q == '\\' || *q == '/') last = q + 1;
    game_file(last, out);
    {
        const char *sub = sound_sub(last);
        if (sub) {
            char cand[MAX_PATH];
            if (strchr(sub, '\\') || strchr(sub, '/')) snprintf(cand, MAX_PATH, "%s", sub);
            else game_file(sub, cand);
            if (plat_file_exists(cand)) {
                static int said[4];
                int k = ieq(last, "MIDI.DRV") ? 0 : ieq(last, "DIGI.DRV") ? 1 : ieq(last, "PRESETS.DEF") ? 3 : 2;
                if (!said[k]++) logmsg("sound: %s served from %s\n", last, cand);
                strcpy(out, cand);
            }
        }
    }
    size_t n = strlen(last);
    if (getenv("POP2_KEYS") && n > 4 && ieq(last + n - 4, ".SAV")) {
        char exe[MAX_PATH]; plat_exe_dir(exe);
        PATH_FMT(out, "%s/test_%s", exe, last);
    }
}
typedef struct { uint16_t seg, paras; int used; } Blk;
static Blk blks[256]; static int nblk;
static void mem_init(uint16_t first_free) {
    nblk = 2;
    blks[0].seg = PSPSEG; blks[0].paras = (uint16_t)(first_free - PSPSEG); blks[0].used = 1;
    blks[1].seg = first_free; blks[1].paras = (uint16_t)(MEMTOP - first_free); blks[1].used = 0;
}
static int dos_alloc(uint16_t paras, uint16_t *seg, uint16_t *largest) {
    uint16_t big = 0;
    for (int i = 0; i < nblk; i++) {
        if (blks[i].used) continue;
        if (blks[i].paras >= paras) {
            if (blks[i].paras > paras) {
                memmove(&blks[i + 2], &blks[i + 1], sizeof(Blk) * (nblk - i - 1)); nblk++;
                blks[i + 1].seg = (uint16_t)(blks[i].seg + paras); blks[i + 1].paras = (uint16_t)(blks[i].paras - paras); blks[i + 1].used = 0;
                blks[i].paras = paras;
            }
            blks[i].used = 1; *seg = blks[i].seg; return 1;
        }
        if (blks[i].paras > big) big = blks[i].paras;
    }
    *largest = big; return 0;
}
static void dos_free(uint16_t seg) {
    for (int i = 0; i < nblk; i++) if (blks[i].seg == seg) { blks[i].used = 0; break; }
    for (int i = 0; i + 1 < nblk; ) {
        if (!blks[i].used && !blks[i + 1].used) { blks[i].paras = (uint16_t)(blks[i].paras + blks[i + 1].paras); memmove(&blks[i + 1], &blks[i + 2], sizeof(Blk) * (nblk - i - 2)); nblk--; }
        else i++;
    }
}
static int dos_resize(uint16_t seg, uint16_t paras, uint16_t *maxp) {
    for (int i = 0; i < nblk; i++) if (blks[i].seg == seg) {
        uint16_t avail = blks[i].paras;
        if (i + 1 < nblk && !blks[i + 1].used) avail = (uint16_t)(avail + blks[i + 1].paras);
        if (paras > avail) { *maxp = avail; return 0; }
        uint16_t total = avail;
        if (i + 1 < nblk && !blks[i + 1].used) { memmove(&blks[i + 1], &blks[i + 2], sizeof(Blk) * (nblk - i - 2)); nblk--; }
        blks[i].paras = paras;
        if (total > paras) { memmove(&blks[i + 2], &blks[i + 1], sizeof(Blk) * (nblk - i - 1)); nblk++;
            blks[i + 1].seg = (uint16_t)(seg + paras); blks[i + 1].paras = (uint16_t)(total - paras); blks[i + 1].used = 0; }
        return 1;
    }
    *maxp = 0; return 0;
}
static void set_cf(int c) { FC = (uint8_t)(c != 0); }
static uint16_t dta_seg = PSPSEG, dta_off = 0x80;

static void dos_out(char c) {
    static char line[256]; static int n;
    if (c == '\r') return;
    if (c == '\n' || n >= (int)sizeof line - 1) { line[n] = 0; if (n) logmsg("DOS output: %s\n", line); n = 0; return; }
    line[n++] = c;
}
static void idle_poll(void) {
    static long tick_seen = -1; static int polls;
    if (!lever_on(LEVER_SLEEP)) return;
    if (n_irq8 != tick_seen) { tick_seen = n_irq8; polls = 0; }
    if (++polls < 16) return;
    tick_sleep();
}
static void dos21(void) {
    uint8_t ah = AH;
    switch (ah) {
    case 0x02: dos_out((char)DL); return;
    case 0x06: case 0x07: case 0x08: case 0x01: {
        static int ext_pending; static uint8_t ext_scan; static int keylog = -1;
        if (ah == 0x06 && DL != 0xFF) { dos_out((char)DL); return; }
        if (keylog < 0) keylog = getenv("POP2_KEYLOG") != NULL;
        if (keylog) { static uint32_t lt; static int n; n++; if (plat_ms() - lt > 1000) { logmsg("DOS key polls: %d\n", n); n = 0; lt = plat_ms(); } }
        if (ah != 0x06) while (!ext_pending && bk_head == bk_tail) { plat_sleep_ms(5); tick_check(); pump(); }
        if (ext_pending) { AL = ext_scan; ext_pending = 0; FZ = 0; return; }
        if (bk_head == bk_tail) { idle_poll(); AL = 0; FZ = 1; return; }
        uint16_t k = bios_kbuf[bk_head++ & 63];
        AL = (uint8_t)k; FZ = 0;
        if (keylog) logmsg("DOS key %04x (fn %02x) in %s\n", k, ah, cur_fn);
        if (AL == 0) { ext_pending = 1; ext_scan = (uint8_t)(k >> 8); }
        return;
    }
    case 0x09: { char s[256]; int i = 0; while (i < 255 && RB(DS, (uint16_t)(DX + i)) != '$') { s[i] = (char)RB(DS, (uint16_t)(DX + i)); i++; } s[i] = 0; logmsg("DOS print: %s\n", s); return; }
    case 0x0B: AL = (bk_head != bk_tail) ? 0xFF : 0; return;
    case 0x0C: if (getenv("POP2_KEYLOG") && bk_head != bk_tail) logmsg("DOS flush drops %d keys\n", bk_tail - bk_head);
        bk_head = bk_tail;
        if (AL == 0x06 || AL == 0x07 || AL == 0x08 || AL == 0x01 || AL == 0x0A) { AH = AL; dos21(); }
        return;
    case 0x0E: AL = 26; return;
    case 0x19: AL = 2; return;
    case 0x1A: dta_seg = DS; dta_off = DX; return;
    case 0x25: WW(0, AL * 4, DX); WW(0, AL * 4 + 2, DS); logmsg("set vector %02x -> %04x:%04x\n", AL, DS, DX); return;
    case 0x2A: { time_t tt = time(NULL); struct tm *t = localtime(&tt); CX = (uint16_t)(t->tm_year + 1900); DH = (uint8_t)(t->tm_mon + 1); DL = (uint8_t)t->tm_mday; AL = (uint8_t)t->tm_wday; return; }
    case 0x2C: { time_t tt = time(NULL); struct tm *t = localtime(&tt); CH = (uint8_t)t->tm_hour; CL = (uint8_t)t->tm_min; DH = (uint8_t)t->tm_sec; DL = (uint8_t)((plat_ms() % 1000) / 10); return; }
    case 0x2F: ES = dta_seg; BX = dta_off; return;
    case 0x30: AX = 0x0005; BX = 0; CX = 0; return;
    case 0x33: DL = 0; return;
    case 0x35: BX = RW(0, AL * 4); ES = RW(0, AL * 4 + 2); return;
    case 0x36: AX = 64; BX = 30000; CX = 512; DX = 60000; return;
    case 0x3B: set_cf(0); return;
    case 0x3C: case 0x3D: {
        char p[MAX_PATH]; host_path(DS, DX, p);
        int h = 5; while (h < 64 && DOS_OPEN(h)) h++;
        if (h >= 64) { AX = 4; set_cf(1); return; }
        int rdonly = ah == 0x3D && (AL & 3) == 0;
        if (!rdonly) cache_drop(p);
        CachedFile *c = NULL; FILE *f = NULL;
        if (rdonly) TIMED(io_us, c = cache_get(p));
        if (c) c->refs++;
        else {
            const char *mode = (ah == 0x3C) ? "w+b" : (rdonly ? "rb" : "r+b");
            TIMED(io_us, f = fopen(p, mode));
            if (!f && ah == 0x3D && !rdonly) TIMED(io_us, f = fopen(p, "rb"));
            if (!f) { logmsg("open failed: %s\n", p); AX = 2; set_cf(1); return; }
        }
        files[h].f = f; files[h].c = c; files[h].pos = 0; snprintf(files[h].name, sizeof files[h].name, "%.259s", p);
        logmsg("open %s -> %d%s t=%u\n", p, h, c ? " cached" : "", plat_ms());
        AX = (uint16_t)h; set_cf(0); return; }
    case 0x3E: if (BX < 64 && DOS_OPEN(BX)) dos_close(BX); set_cf(0); return;
    case 0x3F: {
        if (BX == 0) { AX = 0; set_cf(0); return; }
        if (BX >= 64 || !DOS_OPEN(BX)) { AX = 6; set_cf(1); return; }
        uint32_t dst = LIN(DS, DX); long pos = files[BX].c ? (long)files[BX].pos : ftell(files[BX].f);
        {
            uint32_t live_lo = LIN(SS, SP), live_hi = LIN(SS, 0x9600);
            if (getenv("POP2_MEMLOG") && dst + CX > live_lo && dst < live_hi)
                logmsg("READ OVER LIVE STACK: %u bytes to %04x:%04x (%05x..%05x), ss:sp %04x:%04x bp %04x, in %s\n",
                       CX, DS, DX, dst, dst + CX, SS, SP, BP, cur_fn);
        }
        size_t n;
        if (files[BX].c) {
            CachedFile *c = files[BX].c; uint32_t avail = files[BX].pos < c->size ? c->size - files[BX].pos : 0;
            n = CX < avail ? CX : avail; memcpy(MEM + dst, c->buf + files[BX].pos, n); files[BX].pos += (uint32_t)n;
        } else TIMED(io_us, n = fread(MEM + dst, 1, CX, files[BX].f));
        note_code_read(dst, (uint32_t)pos, (uint32_t)n);
        note_driver_read(files[BX].name, dst, (uint32_t)pos);
        AX = (uint16_t)n; set_cf(0);
        return; }
    case 0x40: {
        if (BX == 1 || BX == 2) { for (uint16_t i = 0; i < CX; i++) dos_out((char)RB(DS, (uint16_t)(DX + i))); AX = CX; set_cf(0); return; }
        if (BX >= 64 || !DOS_OPEN(BX)) { AX = 6; set_cf(1); return; }
        if (files[BX].c) { AX = 5; set_cf(1); return; }
        size_t n = fwrite(MEM + LIN(DS, DX), 1, CX, files[BX].f); AX = (uint16_t)n; set_cf(0); return; }
    case 0x41: { char p[MAX_PATH]; host_path(DS, DX, p); cache_drop(p); remove(p); set_cf(0); return; }
    case 0x42: {
        if (BX >= 64 || !DOS_OPEN(BX)) { AX = 6; set_cf(1); return; }
        long off = (long)(((uint32_t)CX << 16) | DX), p;
        if (files[BX].c) {
            p = (AL == 0 ? 0 : AL == 1 ? (long)files[BX].pos : (long)files[BX].c->size) + off;
            if (p < 0) p = 0;
            files[BX].pos = (uint32_t)p;
        } else {
            TIMED(sk_us, fseek(files[BX].f, off, AL == 0 ? SEEK_SET : AL == 1 ? SEEK_CUR : SEEK_END));
            p = ftell(files[BX].f);
        }
        AX = (uint16_t)p; DX = (uint16_t)(p >> 16); set_cf(0); return; }
    case 0x43: set_cf(0); CX = 0; return;
    case 0x44: if (AL == 0) { DX = (BX < 5) ? 0x80D3 : 0x0002; set_cf(0); return; } set_cf(1); AX = 1; return;
    case 0x47: WB(DS, SI, 0); set_cf(0); return;
    case 0x48: { uint16_t s, big = 0; if (dos_alloc(BX, &s, &big)) { AX = s; set_cf(0); }
        else { logmsg("DOS alloc FAILED: wanted %u paragraphs (%u KB), largest free %u (%u KB), in %s\n",
                      BX, BX / 64, big, big / 64, cur_fn); AX = 8; BX = big; set_cf(1); } return; }
    case 0x49: dos_free(ES); set_cf(0); return;
    case 0x4A: { uint16_t mx = 0; if (dos_resize(ES, BX, &mx)) set_cf(0); else { AX = 8; BX = mx; set_cf(1); } return; }
    case 0x4C:
        logmsg("program exit code %d (last function %s, ss:sp=%04x:%04x)\n", AL, cur_fn, SS, SP);
        for (int i = 0; i < 24; i++) logmsg("   stack +%02x: %04x\n", i * 2, RW(SS, (uint16_t)(SP + i * 2)));
        exit(AL);
    case 0x4E: case 0x4F: AX = 18; set_cf(1); return;
    case 0x58: AX = 0; set_cf(0); return;
    case 0x62: BX = PSPSEG; return;
    }
    logmsg("unhandled INT 21h AH=%02x AL=%02x\n", AH, AL);
    set_cf(1);
}

static void bios_int(int n);
#define XMS_HANDLES 64
static uint8_t *xms_blk[XMS_HANDLES]; static uint32_t xms_size[XMS_HANDLES]; static int xms_lock[XMS_HANDLES];
static uint32_t xms_total_kb = 15 * 1024;
static uint32_t xms_used_kb(void) { uint32_t s = 0; for (int i = 1; i < XMS_HANDLES; i++) s += xms_size[i] / 1024; return s; }
static void xms_call(void) {
    switch (AH) {
    case 0x00: AX = 0x0300; BX = 0x0300; DX = 0; return;
    case 0x08: { uint32_t f = xms_total_kb - xms_used_kb(); AX = (uint16_t)(f > 0xFFFF ? 0xFFFF : f); DX = AX; BL = 0; return; }
    case 0x09: {
        for (int h = 1; h < XMS_HANDLES; h++) if (!xms_blk[h]) {
            if (DX > xms_total_kb - xms_used_kb()) break;
            xms_size[h] = (uint32_t)DX * 1024; xms_blk[h] = calloc(1, xms_size[h] ? xms_size[h] : 1);
            AX = 1; DX = (uint16_t)h; BL = 0; return; }
        AX = 0; BL = 0xA0; return; }
    case 0x0A: if (DX && DX < XMS_HANDLES && xms_blk[DX]) { free(xms_blk[DX]); xms_blk[DX] = NULL; xms_size[DX] = 0; AX = 1; } else { AX = 0; BL = 0xA2; } return;
    case 0x0B: {
        uint32_t len = RW(DS, SI) | ((uint32_t)RW(DS, (uint16_t)(SI + 2)) << 16);
        uint16_t sh = RW(DS, (uint16_t)(SI + 4)); uint32_t so = RW(DS, (uint16_t)(SI + 6)) | ((uint32_t)RW(DS, (uint16_t)(SI + 8)) << 16);
        uint16_t dh = RW(DS, (uint16_t)(SI + 10)); uint32_t dof = RW(DS, (uint16_t)(SI + 12)) | ((uint32_t)RW(DS, (uint16_t)(SI + 14)) << 16);
        uint8_t *src = sh ? (xms_blk[sh] ? xms_blk[sh] + so : NULL) : MEM + LIN(so >> 16, so & 0xFFFF);
        uint8_t *dst = dh ? (xms_blk[dh] ? xms_blk[dh] + dof : NULL) : MEM + LIN(dof >> 16, dof & 0xFFFF);
        if (!src || !dst || (sh && so + len > xms_size[sh]) || (dh && dof + len > xms_size[dh])) { AX = 0; BL = 0xA7; return; }
        memmove(dst, src, len); AX = 1; BL = 0; return; }
    case 0x0C: if (DX < XMS_HANDLES && xms_blk[DX]) { xms_lock[DX]++; AX = 1; DX = 0x0010; BX = 0; } else { AX = 0; BL = 0xA2; } return;
    case 0x0D: if (DX < XMS_HANDLES && xms_blk[DX]) { if (xms_lock[DX]) xms_lock[DX]--; AX = 1; } else { AX = 0; BL = 0xA2; } return;
    case 0x0E: if (DX < XMS_HANDLES && xms_blk[DX]) { AX = 1; BH = (uint8_t)xms_lock[DX]; BL = 32; DX = (uint16_t)(xms_size[DX] / 1024); } else { AX = 0; BL = 0xA2; } return;
    case 0x0F: if (DX < XMS_HANDLES && xms_blk[DX]) { uint32_t n = (uint32_t)BX * 1024; uint8_t *p = realloc(xms_blk[DX], n ? n : 1);
                   if (p) { if (n > xms_size[DX]) memset(p + xms_size[DX], 0, n - xms_size[DX]); xms_blk[DX] = p; xms_size[DX] = n; AX = 1; } else { AX = 0; BL = 0xA0; } }
               else { AX = 0; BL = 0xA2; } return;
    case 0x03: case 0x04: case 0x05: case 0x06: AX = 1; BL = 0; return;
    case 0x07: AX = 1; BL = 0; return;
    case 0x10: AX = 0; BL = 0xB1; DX = 0; return;
    }
    logmsg("unhandled XMS AH=%02x\n", AH); AX = 0; BL = 0x80;
}

static struct {
    Blk blks[256]; int nblk;
    uint8_t *xms[XMS_HANDLES]; uint32_t xms_size[XMS_HANDLES]; int xms_lock[XMS_HANDLES];
    int open[64]; char name[64][260]; long pos[64]; int cached[64];
    uint16_t dta_seg, dta_off; double pit_hz; uint16_t pit_reload;
} cpx;
static void cp_save_ext(void) {
    memcpy(cpx.blks, blks, sizeof blks); cpx.nblk = nblk;
    for (int i = 0; i < XMS_HANDLES; i++) {
        free(cpx.xms[i]); cpx.xms[i] = NULL;
        cpx.xms_size[i] = xms_size[i]; cpx.xms_lock[i] = xms_lock[i];
        if (xms_blk[i] && xms_size[i]) { cpx.xms[i] = malloc(xms_size[i]); memcpy(cpx.xms[i], xms_blk[i], xms_size[i]); }
    }
    for (int i = 0; i < 64; i++) {
        cpx.open[i] = DOS_OPEN(i); cpx.cached[i] = files[i].c != NULL;
        if (cpx.open[i]) { strcpy(cpx.name[i], files[i].name); cpx.pos[i] = files[i].c ? (long)files[i].pos : ftell(files[i].f); }
    }
    cpx.dta_seg = dta_seg; cpx.dta_off = dta_off; cpx.pit_hz = pit_hz; cpx.pit_reload = pit_reload;
}
static void cp_restore_ext(void) {
    memcpy(blks, cpx.blks, sizeof blks); nblk = cpx.nblk;
    for (int i = 0; i < XMS_HANDLES; i++) {
        if (cpx.xms[i]) {
            if (xms_size[i] != cpx.xms_size[i] || !xms_blk[i]) { free(xms_blk[i]); xms_blk[i] = malloc(cpx.xms_size[i]); }
            memcpy(xms_blk[i], cpx.xms[i], cpx.xms_size[i]);
        } else if (xms_blk[i] && !cpx.xms_size[i]) { free(xms_blk[i]); xms_blk[i] = NULL; }
        xms_size[i] = cpx.xms_size[i]; xms_lock[i] = cpx.xms_lock[i];
    }
    for (int i = 0; i < 64; i++) {
        if (DOS_OPEN(i) && (!cpx.open[i] || strcmp(files[i].name, cpx.name[i]) != 0 || (files[i].c != NULL) != cpx.cached[i])) dos_close(i);
        if (cpx.open[i]) {
            if (!DOS_OPEN(i)) {
                if (cpx.cached[i]) { files[i].c = cache_get(cpx.name[i]); if (files[i].c) files[i].c->refs++; }
                if (!files[i].c) { files[i].f = fopen(cpx.name[i], "r+b"); if (!files[i].f) files[i].f = fopen(cpx.name[i], "rb"); }
                strcpy(files[i].name, cpx.name[i]);
            }
            if (files[i].c) files[i].pos = (uint32_t)cpx.pos[i]; else if (files[i].f) fseek(files[i].f, cpx.pos[i], SEEK_SET);
        }
    }
    dta_seg = cpx.dta_seg; dta_off = cpx.dta_off; pit_hz = cpx.pit_hz; pit_reload = cpx.pit_reload;
}

static void bios_call(uint16_t off) {
    if (off == 0x0100) { xms_call(); POP(); POP(); return; }
    bios_int(off & 0xFF);
    POP(); POP(); set_flags(POP());
}
static void bios_int(int n) {
    switch (n) {
    case 0x08: { uint32_t t = RW(0x40, 0x6C) | ((uint32_t)RW(0x40, 0x6E) << 16); t++; WW(0x40, 0x6C, (uint16_t)t); WW(0x40, 0x6E, (uint16_t)(t >> 16)); return; }
    case 0x09: return;
    }
}

void do_int(int n, uint16_t cs, uint16_t ip) {
    (void)cs; (void)ip;
    tick_check();
    uint16_t hseg = RW(0, n * 4 + 2), hoff = RW(0, n * 4);
    switch (n) {
    case 0x21: dos21(); return;
    case 0x10:
        if (AH == 0x00) { video_mode = AL & 0x7F; logmsg("video mode %02x\n", AL); if (video_mode == 0x13) memset(MEM + 0xA0000, 0, 64000); return; }
        if (AH == 0x0F) { AL = (uint8_t)video_mode; AH = 40; BH = 0; return; }
        if (AH == 0x10 && AL == 0x12) { for (int i = 0; i < CX; i++) for (int c = 0; c < 3; c++) pal[(BX + i) & 255][c] = RB(ES, (uint16_t)(DX + i * 3 + c)) & 63; return; }
        if (AH == 0x10 && AL == 0x17) { for (int i = 0; i < CX; i++) for (int c = 0; c < 3; c++) WB(ES, (uint16_t)(DX + i * 3 + c), pal[(BX + i) & 255][c]); return; }
        if (AH == 0x1A) { AL = 0x1A; BX = 0x0008; return; }
        if (AH == 0x12) { BH = 0; BL = 3; return; }
        { static int n10; if (n10++ < 40) logmsg("int10 unhandled ax=%04x bx=%04x cx=%04x dx=%04x\n", AX, BX, CX, DX); }
        return;
    case 0x16:
        if (AH == 0x00 || AH == 0x10) { while (bk_head == bk_tail) { plat_sleep_ms(5); tick_check(); } AX = bios_kbuf[bk_head++ & 63]; return; }
        if (AH == 0x01 || AH == 0x11) { if (bk_head != bk_tail) { AX = bios_kbuf[bk_head & 63]; FZ = 0; } else FZ = 1; return; }
        if (AH == 0x02) { AL = 0; return; }
        return;
    case 0x1A: if (AH == 0) { uint32_t t = RW(0x40, 0x6C) | ((uint32_t)RW(0x40, 0x6E) << 16); CX = (uint16_t)(t >> 16); DX = (uint16_t)t; AL = 0; } return;
    case 0x33: AX = 0; return;
    case 0x67: AH = 0x80; return;
    case 0x2F:
        if (AX == 0x4300) { AL = 0x80; return; }
        if (AX == 0x4310) { ES = BIOSSEG; BX = 0x0100; return; }
        return;
    case 0x11: AX = 0x4426; return;
    case 0x12: AX = 640; return;
    case 0x15: FC = 1; AH = 0x86; return;
    case 0x05: return;
    }
    if (hseg != BIOSSEG && hseg != 0) {
        PUSH(get_flags()); PUSH(cs); PUSH(ip);
        call_far(hseg, hoff);
        return;
    }
    logmsg("unhandled INT %02x AX=%04x\n", n, AX);
}

extern const SpaceTab space_tabs[];

int game_main(const char *cmd) {
    plat_install_crash_handler();
    t_last_tick = t_last_present = plat_us();
    load_settings();
    controls_load();
    char test[MAX_PATH];
    if (cfg_game_dir[0]) strcpy(game_dir, cfg_game_dir); else plat_default_game_dir(game_dir);
    check_files();
    game_file("PRINCE.EXE", test);
    FILE *ef = fopen(test, "rb");
    if (!ef) fatal("PRINCE.EXE not found in %s\n\nRun the PoP2 Launcher and choose the game folder.", game_dir);
    fseek(ef, 0, SEEK_END); long esz = ftell(ef); fseek(ef, 0, SEEK_SET);
    uint8_t *exe = malloc(esz); fread(exe, 1, esz, ef); fclose(ef);
    if (esz != pop2_exe_size || (long)pop2_fp_off + 2 > esz || memcmp(exe + pop2_fp_off, pop2_fp, 2) != 0)
        fatal("PRINCE.EXE in %s is not the '%s' build this program was recompiled from (%ld bytes, expected %ld)",
              game_dir, pop2_version, esz, pop2_exe_size);
    logmsg("PRINCE.EXE: version %s, %ld bytes\n", pop2_version, esz);

    FI = 1;
    MEM = calloc(1, 0x110000);
    init_dispatch();
    for (int i = 0; i < 256; i++) { WW(0, i * 4, (uint16_t)i); WW(0, i * 4 + 2, BIOSSEG); }
    WW(0x40, 0x13, 640);
    uint16_t *h = (uint16_t *)exe;
    uint32_t img0 = h[4] * 16, img1 = h[2] * 512 - (h[1] ? 512 - h[1] : 0);
    memcpy(MEM + LOADSEG * 16, exe + img0, img1 - img0);
    for (const ExePatch *p = pop2_patches; p->len; p++) {
        uint8_t *m = MEM + LOADSEG * 16 + p->off;
        if (memcmp(m, p->new_, p->len) == 0) continue;
        if (memcmp(m, p->old, p->len) != 0) fatal("PRINCE.EXE in %s: root %05x holds other bytes than the '%s' exe", game_dir, p->off, pop2_version);
        memcpy(m, p->new_, p->len);
        logmsg("PRINCE.EXE: root %05x patched, %u bytes\n", p->off, p->len);
    }
    for (int i = 0; i < h[3]; i++) {
        uint16_t off = *(uint16_t *)(exe + h[12] + 4 * i), seg = *(uint16_t *)(exe + h[12] + 4 * i + 2);
        uint32_t a = (uint32_t)(LOADSEG + seg) * 16 + off;
        uint16_t v = (uint16_t)(MEM[a] | (MEM[a + 1] << 8)); v += LOADSEG; MEM[a] = (uint8_t)v; MEM[a + 1] = (uint8_t)(v >> 8);
    }
    uint32_t image_paras = (img1 - img0 + 15) / 16;
    uint16_t first_free = (uint16_t)(LOADSEG + image_paras + h[5]);
    mem_init(first_free);
    logmsg("conventional memory: program ends at %04x, %u KB free below %04x\n",
           first_free, (unsigned)((MEMTOP - first_free) / 64), MEMTOP);
    uint16_t envseg = (uint16_t)(PSPSEG - 0x20);
    WB(PSPSEG, 0, 0xCD); WB(PSPSEG, 1, 0x20); WW(PSPSEG, 2, MEMTOP); WW(PSPSEG, 0x2C, envseg);
    {
        int n = 0;
        if (cmd && *cmd) { WB(PSPSEG, 0x81, ' '); n = 1; for (; cmd[n - 1] && n < 126; n++) WB(PSPSEG, (uint16_t)(0x81 + n), (uint8_t)cmd[n - 1]); }
        WB(PSPSEG, 0x80, (uint8_t)n); WB(PSPSEG, (uint16_t)(0x81 + n), 0x0D);
    }
    static const char env[] = "COMSPEC=C:\\COMMAND.COM\0PATH=C:\\\0\0\x01\0C:\\PRINCE2\\PRINCE.EXE\0";
    memcpy(MEM + envseg * 16, env, sizeof env);
    int driven = 0;
    if (!plat_init(cfg_scale, cfg_fullscreen && !getenv("POP2_KEYS") && !driven, getenv("POP2_KEYS") != NULL || driven)) fatal("could not open a window");
    plat_ready = 1;
    plat_set_title(cfg_filter, cfg_aspect);
    autokeys_init();
    if (getenv("POP2_WATCH")) plat_thread(watchdog, NULL);
    SS = (uint16_t)(LOADSEG + h[7]); SP = h[8]; DS = ES = PSPSEG;
    uint16_t cs = (uint16_t)(LOADSEG + h[11]), ip = h[10];
    logmsg("start %04x:%04x ss:sp %04x:%04x first free %04x\n", cs, ip, SS, SP, first_free);
    call_far(cs, ip);
    logmsg("entry returned\n");
    return 0;
}
