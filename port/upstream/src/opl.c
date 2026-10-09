#include <math.h>
#include <stdint.h>
#include <string.h>
#include "levers.h"

#define OPL_RATE 49716.0

typedef struct {
    int am, vib, egt, ksr, mult, ksl, tl, ar, dr, sl, rr, ws;
    uint32_t phase;
    double env;
    int state;
    int key;
    int out, prev;
    int pg_out;
    double atk_k, dec_inc, rel_inc, sl_att, kslpart, freq; int atk_instant;
} Op;

typedef struct {
    int fnum, block, keyon, fb, con;
    Op op[2];
} Chan;

static Chan ch[9];
static uint8_t reg[256];
static int wse, rhythm, am_depth, vib_depth, rhythm_keys;
static uint32_t noise = 1;
static double lfo_am_phase, lfo_vib_phase, resample_pos;
static int timer_status;

static const int slot_ch[32] = { 0, 1, 2, 0, 1, 2, -1, -1, 3, 4, 5, 3, 4, 5, -1, -1,
                                 6, 7, 8, 6, 7, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static const int slot_op[32] = { 0, 0, 0, 1, 1, 1, -1, -1, 0, 0, 0, 1, 1, 1, -1, -1,
                                 0, 0, 0, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static const int mt[16] = { 1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30 };
static const double ksl_tab[16] = { 0, 12, 16, 18.5, 20, 21.5, 22.5, 23.5, 24, 25, 25.5, 26, 26.5, 27, 27.5, 28 };

static double sin_tab[1024], att_tab[9601];
static int tables_ready;

static void init_tables(void) {
    for (int i = 0; i < 1024; i++) sin_tab[i] = sin((i + 0.5) * 3.14159265358979 / 512.0);
    for (int i = 0; i <= 9600; i++) att_tab[i] = pow(10.0, -i / 2000.0);
    tables_ready = 1;
}

static void chan_cache(Chan *c);
void opl_reset(void) {
    memset(ch, 0, sizeof ch); memset(reg, 0, sizeof reg);
    wse = rhythm = am_depth = vib_depth = rhythm_keys = 0; timer_status = 0;
    for (int c = 0; c < 9; c++) { for (int o = 0; o < 2; o++) { ch[c].op[o].env = 511; ch[c].op[o].state = 3; } chan_cache(&ch[c]); }
    if (!tables_ready) init_tables();
}

static void op_cache(Op *o, const Chan *c);
static void key_on(Op *o, int src) {
    if (!o->key) { o->state = 0; o->phase = 0; }
    o->key |= src;
}
static void key_off(Op *o, int src) {
    if (o->key) { o->key &= ~src; if (!o->key) o->state = 3; }
}

static void set_rhythm_keys(int v) {
    Op *bd0 = &ch[6].op[0], *bd1 = &ch[6].op[1], *hh = &ch[7].op[0], *sd = &ch[7].op[1], *tom = &ch[8].op[0], *cym = &ch[8].op[1];
    if (rhythm) {
        if (v & 0x10) { key_on(bd0, 2); key_on(bd1, 2); } else { key_off(bd0, 2); key_off(bd1, 2); }
        if (v & 0x08) key_on(sd, 2); else key_off(sd, 2);
        if (v & 0x04) key_on(tom, 2); else key_off(tom, 2);
        if (v & 0x02) key_on(cym, 2); else key_off(cym, 2);
        if (v & 0x01) key_on(hh, 2); else key_off(hh, 2);
    } else {
        key_off(bd0, 2); key_off(bd1, 2); key_off(sd, 2); key_off(tom, 2); key_off(cym, 2); key_off(hh, 2);
    }
}

void opl_write(int r, int v) {
    r &= 0xFF; v &= 0xFF;
    if (!tables_ready) opl_reset();
    reg[r] = (uint8_t)v;
    if (r == 0x01) { wse = (v >> 5) & 1; return; }
    if (r == 0x04) { if (v & 0x80) timer_status = 0; else if ((v & 1) && !(v & 0x40)) timer_status |= 0xC0; else if ((v & 2) && !(v & 0x20)) timer_status |= 0xA0; return; }
    if (r == 0xBD) { am_depth = (v >> 7) & 1; vib_depth = (v >> 6) & 1; rhythm = (v >> 5) & 1; set_rhythm_keys(v); return; }
    int s = r & 0x1F;
    if (r >= 0x20 && r < 0xA0 && slot_ch[s] >= 0) {
        Chan *c = &ch[slot_ch[s]]; Op *o = &c->op[slot_op[s]];
        switch (r & 0xE0) {
        case 0x20: o->am = (v >> 7) & 1; o->vib = (v >> 6) & 1; o->egt = (v >> 5) & 1; o->ksr = (v >> 4) & 1; o->mult = v & 15; break;
        case 0x40: o->ksl = (v >> 6) & 3; o->tl = v & 63; break;
        case 0x60: o->ar = (v >> 4) & 15; o->dr = v & 15; break;
        case 0x80: o->sl = (v >> 4) & 15; o->rr = v & 15; break;
        }
        op_cache(o, c);
        return;
    }
    if (r >= 0xE0 && slot_ch[s] >= 0) { ch[slot_ch[s]].op[slot_op[s]].ws = v & 3; return; }
    if (r >= 0xA0 && r <= 0xA8) { ch[r - 0xA0].fnum = (ch[r - 0xA0].fnum & 0x300) | v; chan_cache(&ch[r - 0xA0]); return; }
    if (r >= 0xB0 && r <= 0xB8) {
        Chan *c = &ch[r - 0xB0];
        c->fnum = (c->fnum & 0xFF) | ((v & 3) << 8); c->block = (v >> 2) & 7; chan_cache(c);
        int k = (v >> 5) & 1;
        if (k && !c->keyon) { key_on(&c->op[0], 1); key_on(&c->op[1], 1); }
        if (!k && c->keyon) { key_off(&c->op[0], 1); key_off(&c->op[1], 1); }
        c->keyon = k;
        return;
    }
    if (r >= 0xC0 && r <= 0xC8) { ch[r - 0xC0].fb = (v >> 1) & 7; ch[r - 0xC0].con = v & 1; return; }
}

void opl_all_off(void) {
    for (int c = 0; c < 9; c++) { key_off(&ch[c].op[0], 3); key_off(&ch[c].op[1], 3); ch[c].keyon = 0; reg[0xB0 + c] &= ~0x20; }
    set_rhythm_keys(0);
}

int opl_status(void) { return timer_status ? (timer_status | 0x80) & 0xE0 : 0; }

static double rate_speed(int eff) {
    if (eff > 60) eff = 60;
    return (4 + (eff & 3)) / 4.0 * ldexp(1.0, (eff >> 2) - 1);
}

static double env_inc(int R, int rof) {
    if (R == 0) return 0;
    double t_ms = 39280.64 / rate_speed(4 * R + rof);
    return 512.0 / (t_ms * OPL_RATE / 1000.0);
}
static void op_cache(Op *o, const Chan *c) {
    int rof = (c->block * 2 + ((c->fnum >> 9) & 1)) >> (o->ksr ? 0 : 2);
    o->atk_instant = 4 * o->ar + rof >= 60;
    o->atk_k = 0;
    if (o->ar && !o->atk_instant) {
        double t_ms = 2826.24 / rate_speed(4 * o->ar + rof);
        o->atk_k = 6.24 / (t_ms * OPL_RATE / 1000.0);
    }
    o->dec_inc = env_inc(o->dr, rof); o->rel_inc = env_inc(o->rr, rof);
    o->sl_att = o->sl == 15 ? 496 : o->sl * 16;
    o->kslpart = 0;
    if (o->ksl) {
        double k = ksl_tab[(c->fnum >> 6) & 15] - 8.0 * (7 - c->block);
        if (k > 0) o->kslpart = k * 0.75 / (o->ksl == 1 ? 2 : o->ksl == 2 ? 4 : 1);
    }
    o->freq = (double)(c->fnum << c->block) * mt[o->mult] / 4.0;
}
static void chan_cache(Chan *c) { op_cache(&c->op[0], c); op_cache(&c->op[1], c); }

static void env_step_ref(Op *o, const Chan *c);
static void env_step(Op *o, const Chan *c) {
    if (!lever_on(LEVER_OPL)) { env_step_ref(o, c); return; }
    double inc;
    switch (o->state) {
    case 0:
        if (o->ar == 0) return;
        if (o->atk_instant) { o->env = 0; o->state = 1; return; }
        o->env -= (o->env + 1.0) * o->atk_k;
        if (o->env <= 0) { o->env = 0; o->state = 1; }
        return;
    case 1: if (o->env >= o->sl_att) { o->state = 2; return; } inc = o->dec_inc; break;
    case 2: if (o->egt) return; inc = o->rel_inc; break;
    default: if (o->env >= 511) return; inc = o->rel_inc; break;
    }
    if (inc == 0) return;
    o->env += inc;
    if (o->state == 1 && o->env >= o->sl_att) { o->env = o->sl_att; o->state = 2; }
    if (o->env > 511) o->env = 511;
}
static void env_step_ref(Op *o, const Chan *c) {
    int rof = (c->block * 2 + ((c->fnum >> 9) & 1)) >> (o->ksr ? 0 : 2);
    double sl = o->sl == 15 ? 496 : o->sl * 16;
    int R;
    switch (o->state) {
    case 0:
        R = o->ar;
        if (R == 0) return;
        if (4 * R + rof >= 60) { o->env = 0; o->state = 1; return; }
        {
            double t_ms = 2826.24 / rate_speed(4 * R + rof);
            double k = 6.24 / (t_ms * OPL_RATE / 1000.0);
            o->env -= (o->env + 1.0) * k;
            if (o->env <= 0) { o->env = 0; o->state = 1; }
        }
        return;
    case 1:
        R = o->dr;
        if (o->env >= sl) { o->state = 2; return; }
        break;
    case 2:
        if (o->egt) return;
        R = o->rr;
        break;
    default:
        R = o->rr;
        break;
    }
    if (R == 0) return;
    double t_ms = 39280.64 / rate_speed(4 * R + rof);
    o->env += 512.0 / (t_ms * OPL_RATE / 1000.0);
    if (o->state == 1 && o->env >= sl) { o->env = sl; o->state = 2; }
    if (o->env > 511) o->env = 511;
}

static int op_calc(Op *o, const Chan *c, int phase, double am) {
    double att = o->env * 0.1875 + o->tl * 0.75;
    if (lever_on(LEVER_OPL)) {
        if (o->state == 3 && o->env >= 511) {
            return 0;
        }
        att += o->kslpart;
    } else if (o->ksl) {
        double k = ksl_tab[(c->fnum >> 6) & 15] - 8.0 * (7 - c->block);
        if (k > 0) att += k * 0.75 / (o->ksl == 1 ? 2 : o->ksl == 2 ? 4 : 1);
    }
    if (o->am) att += am;
    if (att >= 96) {
        return 0;
    }
    phase &= 1023;
    double s;
    switch (wse ? o->ws : 0) {
    default: s = sin_tab[phase]; break;
    case 1: s = phase < 512 ? sin_tab[phase] : 0; break;
    case 2: s = fabs(sin_tab[phase]); break;
    case 3: s = (phase & 256) ? 0 : fabs(sin_tab[phase]); break;
    }
    return (int)(s * 4095.0 * att_tab[(int)(att * 100.0)]);
}

static void phase_step(Op *o, const Chan *c, double vib) {
    double f = lever_on(LEVER_OPL) ? o->freq : (double)(c->fnum << c->block) * mt[o->mult] / 4.0;
    if (o->vib) f *= vib;
    o->phase += (uint32_t)f;
}

static int opl_sample(void) {
    lfo_am_phase += 3.7 / OPL_RATE; if (lfo_am_phase >= 1) lfo_am_phase -= 1;
    lfo_vib_phase += 6.07 / OPL_RATE; if (lfo_vib_phase >= 1) lfo_vib_phase -= 1;
    static double am_w, vib_w; static unsigned lfo_n;
    if (!lever_on(LEVER_OPL) || (lfo_n++ & 63) == 0) {
        am_w = 1 - cos(2 * 3.14159265 * lfo_am_phase);
        vib_w = sin(2 * 3.14159265 * lfo_vib_phase);
    }
    double am = (am_depth ? 4.8 : 1.0) * am_w / 2;
    double vib = 1.0 + (vib_depth ? 0.0081 : 0.004) * vib_w;
    uint32_t nb = ((noise >> 14) ^ noise) & 1; noise = (noise >> 1) | (nb << 22);

    int mix = 0;
    int nch = rhythm ? 6 : 9;
    for (int i = 0; i < nch; i++) {
        Chan *c = &ch[i]; Op *m = &c->op[0], *car = &c->op[1];
        env_step(m, c); env_step(car, c);
        int fbmod = c->fb ? (m->out + m->prev) >> (9 - c->fb) : 0;
        int mo = op_calc(m, c, (int)(m->phase >> 9) + fbmod, am);
        m->prev = m->out; m->out = mo;
        int co = op_calc(car, c, (int)(car->phase >> 9) + (c->con ? 0 : mo), am);
        mix += c->con ? mo + co : co;
        phase_step(m, c, vib); phase_step(car, c, vib);
    }
    if (rhythm) {
        Chan *c6 = &ch[6], *c7 = &ch[7], *c8 = &ch[8];
        Op *m = &c6->op[0], *car = &c6->op[1];
        env_step(m, c6); env_step(car, c6);
        int fbmod = c6->fb ? (m->out + m->prev) >> (9 - c6->fb) : 0;
        int mo = op_calc(m, c6, (int)(m->phase >> 9) + fbmod, am);
        m->prev = m->out; m->out = mo;
        int bd = op_calc(car, c6, (int)(car->phase >> 9) + (c6->con ? 0 : mo), am);
        phase_step(m, c6, vib); phase_step(car, c6, vib);
        Op *hh = &c7->op[0], *sd = &c7->op[1], *tom = &c8->op[0], *tc = &c8->op[1];
        env_step(hh, c7); env_step(sd, c7); env_step(tom, c8); env_step(tc, c8);
        int hp = (int)(hh->phase >> 9) & 1023, tp = (int)(tc->phase >> 9) & 1023;
        int b2 = (hp >> 2) & 1, b3 = (hp >> 3) & 1, b7 = (hp >> 7) & 1, b8 = (hp >> 8) & 1;
        int t3 = (tp >> 3) & 1, t5 = (tp >> 5) & 1;
        int x = (b2 ^ b7) | (b3 ^ t5) | (t3 ^ t5);
        int nbit = noise & 1;
        int ph_hh = (x << 9) | ((x ^ nbit) ? 0xD0 : 0x34);
        int ph_sd = (b8 << 9) | ((b8 ^ nbit) << 8);
        int ph_tc = (x << 9) | 0x80;
        int o_hh = op_calc(hh, c7, ph_hh, am);
        int o_sd = op_calc(sd, c7, ph_sd, am);
        int o_tom = op_calc(tom, c8, (int)(tom->phase >> 9), am);
        int o_tc = op_calc(tc, c8, ph_tc, am);
        phase_step(hh, c7, vib); phase_step(sd, c7, vib); phase_step(tom, c8, vib); phase_step(tc, c8, vib);
        mix += 2 * (bd + o_hh + o_sd + o_tom + o_tc);
    }
    return mix;
}

void opl_render(int16_t *buf, int n, int rate) {
    static int cur;
    if (!tables_ready) opl_reset();
    double step = OPL_RATE / rate;
    for (int i = 0; i < n; i++) {
        resample_pos += step;
        int acc = 0, cnt = 0;
        while (resample_pos >= 1.0) { cur = opl_sample(); acc += cur; cnt++; resample_pos -= 1.0; }
        int v = cnt ? acc / cnt : cur;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        buf[i] = (int16_t)v;
    }
}
