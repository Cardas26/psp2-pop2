#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "plat.h"

#define MIX_RATE 44100
static int audio_on;
static int silent(void) { static int v = -1; if (v < 0) v = getenv("POP2_KEYS") != NULL; return v; }
static void snd_fill(int16_t *out, int n);
static void audio_ensure(void) { if (audio_on || silent()) return; audio_on = plat_audio_start(MIX_RATE, snd_fill); }

static int16_t *dbuf; static int dlen, dcap; static uint32_t dpos, dstep;
static volatile int playing, done_flag; static uint32_t done_at;
static uint16_t cb_seg, cb_off;
static int volume = 15;

static void digi_stop(void) { plat_audio_lock(); playing = 0; done_flag = 0; plat_audio_unlock(); }
static int digi_play(uint16_t seg, uint16_t off, uint16_t len, uint16_t rate) {
    if (playing) return 1;
    if (rate < 3000 || rate > 48000) rate = 11025;
    audio_ensure();
    if (!len) return 0;
    if (!audio_on) {
        playing = 1; done_at = plat_ms() + (uint32_t)len * 1000 / rate;
        return 0;
    }
    plat_audio_lock();
    if (len > dcap) { dbuf = realloc(dbuf, (size_t)len * sizeof *dbuf); dcap = len; }
    uint32_t a = LIN(seg, off);
    for (int i = 0; i < len; i++) { int s = (int)MEM[(a + i) & 0xFFFFF] - 128; dbuf[i] = (int16_t)(s * volume / 15 * 256); }
    dlen = len; dpos = 0; dstep = (uint32_t)(((uint64_t)rate << 16) / MIX_RATE); done_flag = 0; playing = 1;
    plat_audio_unlock();
    return 0;
}

int digi_busy(void) { return playing; }
void digi_abort(void) { digi_stop(); }

void snd_poll(void) {
    int done = 0;
    if (playing) done = audio_on ? done_flag : (int32_t)(plat_ms() - done_at) >= 0;
    if (!done) return;
    plat_audio_lock(); playing = 0; done_flag = 0; plat_audio_unlock();
    if (getenv("POP2_SNDLOG")) logmsg("DIGI done t=%u cb=%04x:%04x\n", plat_ms(), cb_seg, cb_off);
    if (cb_seg || cb_off) {
        PUSH(get_flags()); PUSH(0xFFFF); PUSH(0xFFFF);
        uint16_t sax = AX, sbx = BX, scx = CX, sdx = DX, ssi = SI, sdi = DI, sds = DS, ses = ES, sbp = BP;
        call_far(cb_seg, cb_off);
        AX = sax; BX = sbx; CX = scx; DX = sdx; SI = ssi; DI = sdi; DS = sds; ES = ses; BP = sbp;
    }
}

void opl_reset(void); void opl_write(int r, int v); int opl_status(void); void opl_render(int16_t *buf, int n, int rate); void opl_all_off(void);
static int fm_on, fm_index;
static void midi_reset(void);

static void snd_fill(int16_t *out, int n) {
    static int16_t fm[2048];
    memset(out, 0, (size_t)n * sizeof *out);
    while (n > 0) {
        int m = n > 2048 ? 2048 : n;
        if (fm_on) {
            opl_render(fm, m, MIX_RATE);
            {
                static FILE *dump; static int init;
                if (!init) { const char *p = getenv("POP2_FMWAV"); if (p) dump = fopen(p, "wb"); init = 1; }
                if (dump) { fwrite(fm, 2, (size_t)m, dump); fflush(dump); }
            }
            memcpy(out, fm, (size_t)m * sizeof *out);
        }
        if (playing && !done_flag && dbuf) {
            for (int i = 0; i < m; i++) {
                uint32_t idx = dpos >> 16;
                if ((int)idx >= dlen) { done_flag = 1; break; }
                int v = out[i] + dbuf[idx]; out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                dpos += dstep;
            }
        }
        out += m; n -= m;
    }
}
void fm_start(void) {
    if (fm_on || silent()) return;
    opl_reset();
    audio_ensure();
    if (!audio_on) { logmsg("FM: no audio device\n"); return; }
    plat_audio_lock(); fm_on = 1; plat_audio_unlock();
    logmsg("FM synthesis started\n");
}
void fm_port_write(uint16_t port, uint8_t v) {
    if (!fm_on) return;
    if (port == 0x388) fm_index = v;
    else { plat_audio_lock(); opl_write(fm_index, v); plat_audio_unlock(); }
}
void fm_all_notes_off(void) {
    if (!fm_on) { midi_reset(); return; }
    plat_audio_lock(); opl_all_off(); plat_audio_unlock();
}
uint8_t fm_port_read(void) {
    if (!fm_on) return 0xFF;
    plat_audio_lock(); int s = opl_status(); plat_audio_unlock();
    return (uint8_t)s;
}

static void midi_send(uint8_t st, uint8_t d1, uint8_t d2) { if (!silent()) plat_midi_send(st, d1, d2); }
static void midi_reset(void) {
    for (int ch = 0; ch < 16; ch++) { midi_send((uint8_t)(0xB0 | ch), 123, 0); midi_send((uint8_t)(0xB0 | ch), 121, 0); }
}

static void snd_driver_fn(int kind, int fn);
void snd_driver_call(int kind) {
    static int logged[2][256];
    int fn = AL;
    int log = logged[kind - 1][fn & 0xF0 ? fn & 0xF0 : fn]++ < 2 || (kind == 1 && getenv("POP2_SNDLOG"));
    if (log)
        logmsg("%s driver fn %02x  ax=%04x bx=%04x cx=%04x dx=%04x si=%04x di=%04x es=%04x\n",
               kind == 1 ? "DIGI" : "MIDI", fn, AX, BX, CX, DX, SI, DI, ES);
    snd_driver_fn(kind, fn);
    if (log) logmsg("%s driver fn %02x -> ax=%04x\n", kind == 1 ? "DIGI" : "MIDI", fn, AX);
}
static void snd_driver_fn(int kind, int fn) {
    if (kind == 1) {
        switch (fn) {
        case 0: AX = 0; BX = 1; CX = 0x0F50; DX = 0xFFFF; return;
        case 1: AX = 0; BX = 1; CX = 0x0F50; DX = 0xFFFF; return;
        case 2: digi_stop(); return;
        case 3: AX = playing ? 0 : 1; digi_stop(); return;
        case 4: volume = AH ? 15 : 0; AX = 0; return;
        case 5: case 6: return;
        case 7: cb_off = SI; cb_seg = DI; AX = (uint16_t)digi_play(ES, BX, CX, DX); return;
        case 8: AX = 8; return;
        }
        return;
    }
    if (fn < 8) {
        if (fn == 6 && getenv("POP2_MIDILOG")) {
            uint32_t a = LIN(ES, BX);
            logmsg("MIDI fn6 buffer at %04x:%04x len=%02x:", ES, BX, MEM[a]);
            for (int i = 0; i < 24; i++) logmsg(" %02x", MEM[(a + i) & 0xFFFFF]);
            logmsg("\n");
        }
        if (fn == 2 || fn == 4) midi_reset();
        AX = 0; return;
    }
    uint8_t st = (uint8_t)((fn & 0xF0) | (AH & 0x0F));
    if (getenv("POP2_MIDILOG")) { static int n; if (n++ < 40) logmsg("MIDIEV %02x %02x %02x\n", st, DL & 0x7F, DH & 0x7F); }
    switch (fn & 0xF0) {
    case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: midi_send(st, DL & 0x7F, DH & 0x7F); break;
    case 0xC0: case 0xD0: midi_send(st, DL & 0x7F, 0); break;
    }
}
