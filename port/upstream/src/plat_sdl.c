#include "plat.h"
#include "levers.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <sys/stat.h>
#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>
#include <psp2/message_dialog.h>
#include "diag.h"
#ifdef DIAGNOSTICS
#include "vshot/vshot.h"
#endif
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;
unsigned int _newlib_heap_size_user = 96 * 1024 * 1024;
#endif

static SDL_Window *win; static SDL_Renderer *ren;
static SDL_Texture *tex[2], *tex3x[2], *target, *lcd_tex;
static int lcd_w, lcd_h;
static int target_w, target_h, hidden_run;
#ifndef __vita__
static int full;
#endif
static SDL_GameController *pad;
static void (*audio_fill)(int16_t *, int); static SDL_AudioDeviceID adev;

static uint64_t perf_us(void) { uint64_t c = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency(); return c / f * 1000000ull + c % f * 1000000ull / f; }
uint64_t plat_us(void) { return perf_us(); }
uint32_t plat_ms(void) { return SDL_GetTicks(); }
void plat_sleep_ms(int ms) { SDL_Delay((Uint32)ms); }
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
void plat_sleep_us(int us) { sceKernelDelayThread((SceUInt)us); }
#else
#include <unistd.h>
void plat_sleep_us(int us) { usleep((useconds_t)us); }
#endif

static SDL_Texture *make_tex(int w, int h, int linear) {
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    if (t) SDL_SetTextureScaleMode(t, linear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    return t;
}
int plat_init(int scale, int fullscreen, int hidden) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) { logmsg("SDL_Init: %s\n", SDL_GetError()); return 0; }
    hidden_run = hidden;
#ifdef __vita__
    (void)scale; (void)fullscreen;
    win = SDL_CreateWindow("Prince of Persia 2", 0, 0, 960, 544, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN);
#else
    Uint32 flags = hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN;
    if (fullscreen) { flags |= SDL_WINDOW_FULLSCREEN_DESKTOP; full = 1; }
    win = SDL_CreateWindow("Prince of Persia 2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 320 * scale, 240 * scale, flags | SDL_WINDOW_RESIZABLE);
#endif
    if (!win) { logmsg("SDL_CreateWindow: %s\n", SDL_GetError()); return 0; }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | (lever_on(LEVER_VSYNC) ? SDL_RENDERER_PRESENTVSYNC : 0));
    if (ren) { SDL_RendererInfo ri; if (!SDL_GetRendererInfo(ren, &ri)) logmsg("renderer %s vsync %d\n", ri.name, (ri.flags & SDL_RENDERER_PRESENTVSYNC) != 0); }
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) { logmsg("SDL_CreateRenderer: %s\n", SDL_GetError()); return 0; }
    SDL_RendererInfo ri; if (SDL_GetRendererInfo(ren, &ri) == 0) logmsg("video: SDL %s renderer\n", ri.name);
    for (int i = 0; i < 2; i++) tex[i] = make_tex(320, 200, i);
    if (!tex[0] || !tex[1]) { logmsg("SDL_CreateTexture: %s\n", SDL_GetError()); return 0; }
    for (int i = 0; i < SDL_NumJoysticks() && !pad; i++) if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); if (pad) logmsg("controller: %s\n", SDL_GameControllerName(pad)); }
    return 1;
}
enum { F_SHARP, F_PIXEL, F_SOFT, F_LCD, F_SMOOTH };
static const char *const filter_names[] = { "Sharp", "Pixel", "Soft", "LCD", "Smooth HQ" };
enum { V_43, V_WIDE };
static const char *const view_names[] = { "4:3", "Wide" };
int plat_aspect_modes(void) { return 2; }
const char *plat_aspect_name(int i) { return i >= 0 && i < plat_aspect_modes() ? view_names[i] : "?"; }
const char *plat_filter_name(int i) { return i >= 0 && i < plat_filter_modes() ? filter_names[i] : "?"; }
void plat_set_title(int filter, int aspect) {
    char t[128]; snprintf(t, sizeof t, "Prince of Persia 2   [%s, %s  -  F7 filter, F8 view, F11 fullscreen]", plat_filter_name(filter), plat_aspect_name(aspect));
    if (win) SDL_SetWindowTitle(win, t);
}

static void scale3x(const uint32_t *s, uint32_t *d) {
    for (int y = 0; y < 200; y++) {
        const uint32_t *r0 = s + (y > 0 ? y - 1 : y) * 320, *r1 = s + y * 320, *r2 = s + (y < 199 ? y + 1 : y) * 320;
        uint32_t *o0 = d + (y * 3) * 960, *o1 = o0 + 960, *o2 = o1 + 960;
        for (int x = 0; x < 320; x++) {
            int xl = x > 0 ? x - 1 : x, xr = x < 319 ? x + 1 : x;
            uint32_t A = r0[xl], B = r0[x], C = r0[xr], D = r1[xl], E = r1[x], F = r1[xr], G = r2[xl], H = r2[x], I = r2[xr];
            uint32_t e0 = E, e1 = E, e2 = E, e3 = E, e5 = E, e6 = E, e7 = E, e8 = E;
            if (B != H && D != F) {
                e0 = D == B ? D : E; e1 = (D == B && E != C) || (B == F && E != A) ? B : E; e2 = B == F ? F : E;
                e3 = (D == B && E != G) || (D == H && E != A) ? D : E; e5 = (B == F && E != I) || (H == F && E != C) ? F : E;
                e6 = D == H ? D : E; e7 = (D == H && E != I) || (H == F && E != G) ? H : E; e8 = H == F ? F : E;
            }
            o0[x * 3] = e0; o0[x * 3 + 1] = e1; o0[x * 3 + 2] = e2;
            o1[x * 3] = e3; o1[x * 3 + 1] = E;  o1[x * 3 + 2] = e5;
            o2[x * 3] = e6; o2[x * 3 + 1] = e7; o2[x * 3 + 2] = e8;
        }
    }
}
static uint8_t last_frame[320 * 200], last_pal[256][3]; static int last_filter, last_aspect;
#include "osd_font.h"
static char osd_text[96]; static uint32_t osd_until; static SDL_Texture *osd_tex;
void plat_osd(const char *text, int ms) { snprintf(osd_text, sizeof osd_text, "%s", text); osd_until = plat_ms() + (uint32_t)ms; }
static const unsigned char *osd_glyph(int c) {
    if (c >= 'a' && c <= 'z') c -= 32;
    for (unsigned i = 0; i < sizeof osd_font / sizeof osd_font[0]; i++) if (osd_font[i].c == c) return osd_font[i].row;
    return NULL;
}
static void osd_draw(void) {
    if (!osd_text[0] || (int32_t)(osd_until - plat_ms()) <= 0) return;
    enum { W = 472, H = 9, K = 2 };
    static uint32_t px[W * H];
    if (!osd_tex) {
        osd_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
        if (!osd_tex) return;
        SDL_SetTextureBlendMode(osd_tex, SDL_BLENDMODE_BLEND); SDL_SetTextureScaleMode(osd_tex, SDL_ScaleModeNearest);
    }
    memset(px, 0, sizeof px);
    int x0 = 0, n = 0;
    for (const char *s = osd_text; *s && x0 + 5 <= W; s++, x0 += 6, n++) {
        const unsigned char *g = osd_glyph((unsigned char)*s); if (!g) continue;
        for (int y = 0; y < 7; y++) for (int x = 0; x < 5; x++) if (g[y] & (0x10 >> x)) px[(y + 1) * W + x0 + x + 1] = 0xFF000000u;
    }
    x0 = 0;
    for (const char *s = osd_text; *s && x0 + 5 <= W; s++, x0 += 6) {
        const unsigned char *g = osd_glyph((unsigned char)*s); if (!g) continue;
        for (int y = 0; y < 7; y++) for (int x = 0; x < 5; x++) if (g[y] & (0x10 >> x)) px[y * W + x0 + x] = 0xFFFFFFFFu;
    }
    SDL_UpdateTexture(osd_tex, NULL, px, W * 4);
    SDL_Rect src = { 0, 0, n * 6 + 1, H }, dst = { 8, 8, (n * 6 + 1) * K, H * K };
    SDL_RenderCopy(ren, osd_tex, &src, &dst);
}
int plat_filter_modes(void) {
#ifdef __vita__
    return F_SMOOTH;
#else
    return F_SMOOTH + 1;
#endif
}
static void lcd_draw(const SDL_Rect *dst) {
    if (!lcd_tex || lcd_w != dst->w || lcd_h != dst->h) {
        if (lcd_tex) SDL_DestroyTexture(lcd_tex);
        lcd_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, dst->w, dst->h);
        uint32_t *m = lcd_tex ? malloc((size_t)dst->w * dst->h * 4) : NULL;
        float *xf = m ? malloc(sizeof(float) * 3 * dst->w) : NULL;
        if (!xf) { free(m); if (lcd_tex) SDL_DestroyTexture(lcd_tex); lcd_tex = NULL; logmsg("lcd: no mask %dx%d\n", dst->w, dst->h); return; }
        const double pi = 3.141592654, off[3] = { pi * (1.0 / 2), pi * (1.0 / 2 - 2.0 / 3), pi * (1.0 / 2 - 4.0 / 3) };
        const int brighten_scanlines = 32, brighten_lcd = 12;
        for (int x = 0; x < dst->w; x++) for (int c = 0; c < 3; c++)
            xf[x * 3 + c] = (float)((brighten_lcd + sin(2 * pi * ((x + 0.5) * 320.0 / dst->w) + off[c])) / (brighten_lcd + 1));
        for (int y = 0; y < dst->h; y++) {
            double yf = (brighten_scanlines + sin(2 * pi * ((y + 0.5) * 200.0 / dst->h))) / (brighten_scanlines + 1);
            for (int x = 0; x < dst->w; x++) {
                uint32_t r = (uint32_t)lround(255 * yf * xf[x * 3]), g = (uint32_t)lround(255 * yf * xf[x * 3 + 1]), b = (uint32_t)lround(255 * yf * xf[x * 3 + 2]);
                m[(size_t)y * dst->w + x] = 0xFF000000u | r << 16 | g << 8 | b;
            }
        }
        SDL_UpdateTexture(lcd_tex, NULL, m, dst->w * 4); free(xf); free(m);
        SDL_SetTextureBlendMode(lcd_tex, SDL_BLENDMODE_MOD); SDL_SetTextureScaleMode(lcd_tex, SDL_ScaleModeNearest);
        lcd_w = dst->w; lcd_h = dst->h; logmsg("lcd: mask %dx%d\n", dst->w, dst->h);
    }
    SDL_RenderCopy(ren, lcd_tex, NULL, dst);
}
void plat_present(const uint8_t *frame, const uint8_t pal[256][3], int filter, int aspect) {
    static uint32_t base[320 * 200]; static uint32_t *big;
    if (!ren) return;
    if (frame != last_frame) { memcpy(last_frame, frame, sizeof last_frame); memcpy(last_pal, pal, sizeof last_pal); last_filter = filter; last_aspect = aspect; }
    uint32_t lut[256];
    for (int i = 0; i < 256; i++)
        lut[i] = 0xFF000000u | (uint32_t)(pal[i][0] << 2 | pal[i][0] >> 4) << 16 | (uint32_t)(pal[i][1] << 2 | pal[i][1] >> 4) << 8 | (uint32_t)(pal[i][2] << 2 | pal[i][2] >> 4);
    for (int i = 0; i < 320 * 200; i++) base[i] = lut[frame[i]];
    int w, h; SDL_GetRendererOutputSize(ren, &w, &h);
    int dw = w, dh = h;
    if (aspect == V_43) { dw = w; dh = w * 3 / 4; if (dh > h) { dh = h; dw = h * 4 / 3; } }
    SDL_Rect dst = { (w - dw) / 2, (h - dh) / 2, dw, dh };
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren);
    if (filter == F_SMOOTH) {
        if (!big) big = malloc(960 * 600 * 4);
        if (!tex3x[1]) tex3x[1] = make_tex(960, 600, 1);
        scale3x(base, big);
        SDL_UpdateTexture(tex3x[1], NULL, big, 960 * 4);
        SDL_RenderCopy(ren, tex3x[1], NULL, &dst);
    } else if (filter == F_SHARP) {
        int k = dw / 320 < dh / 200 ? dw / 320 : dh / 200; if (k < 1) k = 1;
        SDL_UpdateTexture(tex[0], NULL, base, 320 * 4);
        if (k == 1) { SDL_RenderCopy(ren, tex[0], NULL, &dst); }
        else {
            if (!target || target_w != 320 * k || target_h != 200 * k) {
                if (target) SDL_DestroyTexture(target);
                target = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 320 * k, 200 * k);
                if (target) SDL_SetTextureScaleMode(target, SDL_ScaleModeLinear);
                target_w = 320 * k; target_h = 200 * k;
            }
            if (target) { SDL_SetRenderTarget(ren, target); SDL_RenderCopy(ren, tex[0], NULL, NULL); SDL_SetRenderTarget(ren, NULL); SDL_RenderCopy(ren, target, NULL, &dst); }
            else SDL_RenderCopy(ren, tex[0], NULL, &dst);
        }
    } else {
        SDL_Texture *t = tex[filter == F_SOFT];
        SDL_UpdateTexture(t, NULL, base, 320 * 4);
        SDL_RenderCopy(ren, t, NULL, &dst);
        if (filter == F_LCD) lcd_draw(&dst);
    }
    osd_draw();
    SDL_RenderPresent(ren);
#ifdef __vita__
    diag_alive();
#endif
}

int plat_text_input(const char *title, const char *initial, char *out, int n) {
    out[0] = 0;
#ifdef __vita__
    static SceWChar16 wtitle[64], winit[64], wbuf[64];
    int i;
    for (i = 0; title[i] && i < 63; i++) wtitle[i] = (SceWChar16)(unsigned char)title[i];
    wtitle[i] = 0;
    for (i = 0; initial[i] && i < 63; i++) winit[i] = (SceWChar16)(unsigned char)initial[i];
    winit[i] = 0;
    memset(wbuf, 0, sizeof wbuf);
    sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
    SceImeDialogParam prm; sceImeDialogParamInit(&prm);
    prm.supportedLanguages = 0x0001FFFF; prm.languagesForced = SCE_TRUE;
    prm.type = SCE_IME_TYPE_BASIC_LATIN; prm.option = SCE_IME_OPTION_NO_ASSISTANCE;
    prm.title = wtitle; prm.maxTextLength = (SceUInt32)(n - 1 < 63 ? n - 1 : 63); prm.initialText = winit; prm.inputTextBuffer = wbuf;
    int rc = sceImeDialogInit(&prm);
    if (rc < 0) { logmsg("text input: sceImeDialogInit %08x\n", rc); return -1; }
    SceCommonDialogStatus st;
    while ((st = sceImeDialogGetStatus()) == SCE_COMMON_DIALOG_STATUS_RUNNING) {
        SDL_Event e; while (SDL_PollEvent(&e)) { }
        plat_present(last_frame, last_pal, last_filter, last_aspect);
        SDL_Delay(16);
    }
    SceImeDialogResult res; memset(&res, 0, sizeof res);
    sceImeDialogGetResult(&res);
    sceImeDialogTerm();
    if (st != SCE_COMMON_DIALOG_STATUS_FINISHED || res.button != SCE_IME_DIALOG_BUTTON_ENTER) { logmsg("text input: cancelled\n"); return -1; }
    int len = 0;
    for (i = 0; wbuf[i] && len < n - 1; i++) if (wbuf[i] < 128) out[len++] = (char)wbuf[i];
    out[len] = 0;
    logmsg("text input: %d chars\n", len);
    return len;
#else
    (void)title; (void)initial; (void)n; return -1;
#endif
}

static int sdl_to_scan(SDL_Scancode s) {
    static const struct { SDL_Scancode s; int pc; } map[] = {
        { SDL_SCANCODE_ESCAPE, 0x01 }, { SDL_SCANCODE_1, 0x02 }, { SDL_SCANCODE_2, 0x03 }, { SDL_SCANCODE_3, 0x04 }, { SDL_SCANCODE_4, 0x05 },
        { SDL_SCANCODE_5, 0x06 }, { SDL_SCANCODE_6, 0x07 }, { SDL_SCANCODE_7, 0x08 }, { SDL_SCANCODE_8, 0x09 }, { SDL_SCANCODE_9, 0x0A },
        { SDL_SCANCODE_0, 0x0B }, { SDL_SCANCODE_MINUS, 0x0C }, { SDL_SCANCODE_EQUALS, 0x0D }, { SDL_SCANCODE_BACKSPACE, 0x0E }, { SDL_SCANCODE_TAB, 0x0F },
        { SDL_SCANCODE_Q, 0x10 }, { SDL_SCANCODE_W, 0x11 }, { SDL_SCANCODE_E, 0x12 }, { SDL_SCANCODE_R, 0x13 }, { SDL_SCANCODE_T, 0x14 },
        { SDL_SCANCODE_Y, 0x15 }, { SDL_SCANCODE_U, 0x16 }, { SDL_SCANCODE_I, 0x17 }, { SDL_SCANCODE_O, 0x18 }, { SDL_SCANCODE_P, 0x19 },
        { SDL_SCANCODE_LEFTBRACKET, 0x1A }, { SDL_SCANCODE_RIGHTBRACKET, 0x1B }, { SDL_SCANCODE_RETURN, 0x1C }, { SDL_SCANCODE_LCTRL, 0x1D },
        { SDL_SCANCODE_A, 0x1E }, { SDL_SCANCODE_S, 0x1F }, { SDL_SCANCODE_D, 0x20 }, { SDL_SCANCODE_F, 0x21 }, { SDL_SCANCODE_G, 0x22 },
        { SDL_SCANCODE_H, 0x23 }, { SDL_SCANCODE_J, 0x24 }, { SDL_SCANCODE_K, 0x25 }, { SDL_SCANCODE_L, 0x26 }, { SDL_SCANCODE_SEMICOLON, 0x27 },
        { SDL_SCANCODE_APOSTROPHE, 0x28 }, { SDL_SCANCODE_GRAVE, 0x29 }, { SDL_SCANCODE_LSHIFT, 0x2A }, { SDL_SCANCODE_BACKSLASH, 0x2B },
        { SDL_SCANCODE_Z, 0x2C }, { SDL_SCANCODE_X, 0x2D }, { SDL_SCANCODE_C, 0x2E }, { SDL_SCANCODE_V, 0x2F }, { SDL_SCANCODE_B, 0x30 },
        { SDL_SCANCODE_N, 0x31 }, { SDL_SCANCODE_M, 0x32 }, { SDL_SCANCODE_COMMA, 0x33 }, { SDL_SCANCODE_PERIOD, 0x34 }, { SDL_SCANCODE_SLASH, 0x35 },
        { SDL_SCANCODE_RSHIFT, 0x36 }, { SDL_SCANCODE_KP_MULTIPLY, 0x37 }, { SDL_SCANCODE_LALT, 0x38 }, { SDL_SCANCODE_SPACE, 0x39 }, { SDL_SCANCODE_CAPSLOCK, 0x3A },
        { SDL_SCANCODE_F1, 0x3B }, { SDL_SCANCODE_F2, 0x3C }, { SDL_SCANCODE_F3, 0x3D }, { SDL_SCANCODE_F4, 0x3E }, { SDL_SCANCODE_F5, 0x3F },
        { SDL_SCANCODE_F6, 0x40 }, { SDL_SCANCODE_F7, 0x41 }, { SDL_SCANCODE_F8, 0x42 }, { SDL_SCANCODE_F9, 0x43 }, { SDL_SCANCODE_F10, 0x44 },
        { SDL_SCANCODE_NUMLOCKCLEAR, 0x45 }, { SDL_SCANCODE_SCROLLLOCK, 0x46 }, { SDL_SCANCODE_KP_7, 0x47 }, { SDL_SCANCODE_KP_8, 0x48 }, { SDL_SCANCODE_KP_9, 0x49 },
        { SDL_SCANCODE_KP_MINUS, 0x4A }, { SDL_SCANCODE_KP_4, 0x4B }, { SDL_SCANCODE_KP_5, 0x4C }, { SDL_SCANCODE_KP_6, 0x4D }, { SDL_SCANCODE_KP_PLUS, 0x4E },
        { SDL_SCANCODE_KP_1, 0x4F }, { SDL_SCANCODE_KP_2, 0x50 }, { SDL_SCANCODE_KP_3, 0x51 }, { SDL_SCANCODE_KP_0, 0x52 }, { SDL_SCANCODE_KP_PERIOD, 0x53 },
        { SDL_SCANCODE_F11, 0x57 }, { SDL_SCANCODE_F12, 0x58 },
        { SDL_SCANCODE_KP_ENTER, 0x11C }, { SDL_SCANCODE_RCTRL, 0x11D }, { SDL_SCANCODE_KP_DIVIDE, 0x135 }, { SDL_SCANCODE_RALT, 0x138 },
        { SDL_SCANCODE_HOME, 0x147 }, { SDL_SCANCODE_UP, 0x148 }, { SDL_SCANCODE_PAGEUP, 0x149 }, { SDL_SCANCODE_LEFT, 0x14B }, { SDL_SCANCODE_RIGHT, 0x14D },
        { SDL_SCANCODE_END, 0x14F }, { SDL_SCANCODE_DOWN, 0x150 }, { SDL_SCANCODE_PAGEDOWN, 0x151 }, { SDL_SCANCODE_INSERT, 0x152 }, { SDL_SCANCODE_DELETE, 0x153 },
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) if (map[i].s == s) return map[i].pc;
    return 0;
}
int plat_key_ascii(int scan, int shift, int ctrl) {
    static const char lo[] = "\0\033" "1234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
    static const char up[] = "\0\033" "!@#$%^&*()_+\b\tQWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";
    int s = scan & 0xFF;
    if (scan & 0x100) return s == 0x1C ? '\r' : 0;
    if (s >= (int)sizeof lo - 1) return 0;
    int c = shift ? up[s] : lo[s];
    if (ctrl && c >= 'a' && c <= 'z') return c - 'a' + 1;
    if (ctrl && c >= 'A' && c <= 'Z') return c - 'A' + 1;
    return c;
}
static void toggle_fullscreen(void) {
#ifndef __vita__
    full = !full; SDL_SetWindowFullscreen(win, full ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
#endif
}
int plat_pump(void) {
    SDL_Event e; int quit = 0;
#ifdef __vita__
    diag_alive();
#endif
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: quit = 1; break;
        case SDL_WINDOWEVENT: if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) rt_release_keys(); break;
        case SDL_KEYDOWN: case SDL_KEYUP: {
            if (e.key.repeat) break;
            int up = e.type == SDL_KEYUP; SDL_Keymod m = SDL_GetModState();
            if (!up && e.key.keysym.scancode == SDL_SCANCODE_F11) { toggle_fullscreen(); break; }
            if (!up && e.key.keysym.scancode == SDL_SCANCODE_RETURN && (m & KMOD_ALT)) { toggle_fullscreen(); break; }
            if (!up && e.key.keysym.scancode == SDL_SCANCODE_F4 && (m & KMOD_ALT)) { quit = 1; break; }
            if (!up && e.key.keysym.scancode == SDL_SCANCODE_F7) { rt_cycle_filter(); break; }
            if (!up && e.key.keysym.scancode == SDL_SCANCODE_F8) { rt_cycle_aspect(); break; }
            int scan = sdl_to_scan(e.key.keysym.scancode); if (!scan) break;
            int asc = up ? 0 : plat_key_ascii(scan, (m & KMOD_SHIFT) != 0, (m & KMOD_CTRL) != 0);
            if (m & KMOD_ALT) asc = 0;
            rt_key_event(scan, up, asc); break; }
        case SDL_CONTROLLERDEVICEADDED: if (!pad) { pad = SDL_GameControllerOpen(e.cdevice.which); if (pad) logmsg("controller: %s\n", SDL_GameControllerName(pad)); } break;
        case SDL_CONTROLLERDEVICEREMOVED: if (pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == e.cdevice.which) { SDL_GameControllerClose(pad); pad = NULL; } break;
        }
    }
    return quit;
}
int plat_pad_read(PlatPad *p) {
    if (!pad) return 0;
#define B(x) SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_##x)
#define A(x) SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_##x)
    const int dz = 14000;
    p->up = B(DPAD_UP); p->down = B(DPAD_DOWN); p->left = B(DPAD_LEFT); p->right = B(DPAD_RIGHT);
    p->b_cross = B(A); p->b_circle = B(B); p->b_square = B(X); p->b_tri = B(Y);
    p->b_l = B(LEFTSHOULDER); p->b_r = B(RIGHTSHOULDER); p->b_select = B(BACK); p->b_start = B(START);
    p->rs_up = A(RIGHTY) < -dz; p->rs_down = A(RIGHTY) > dz; p->rs_left = A(RIGHTX) < -dz; p->rs_right = A(RIGHTX) > dz;
    p->ls_up = A(LEFTY) < -dz; p->ls_down = A(LEFTY) > dz; p->ls_left = A(LEFTX) < -dz; p->ls_right = A(LEFTX) > dz;
#if defined(__vita__) && defined(DIAGNOSTICS)
    {
        uint32_t raw = (B(LEFTSHOULDER) ? SCE_CTRL_LTRIGGER : 0) | (B(RIGHTSHOULDER) ? SCE_CTRL_RTRIGGER : 0) | (B(A) ? SCE_CTRL_CROSS : 0);
        if (vshot_combo(raw)) say("shot  %d\n", vshot_take(DATA_DIR));
        if ((raw & VSHOT_COMBO) == VSHOT_COMBO) { p->b_l = p->b_r = p->b_cross = 0; }
    }
#endif
#undef B
#undef A
    return 1;
}

static void SDLCALL audio_cb(void *ud, Uint8 *stream, int len) { (void)ud; audio_fill((int16_t *)stream, len / 2); }
int plat_audio_start(int rate, void (*fill)(int16_t *out, int n)) {
    if (adev) return 1;
    audio_fill = fill;
    SDL_AudioSpec want, have; memset(&want, 0, sizeof want);
    want.freq = rate; want.format = AUDIO_S16SYS; want.channels = 1; want.samples = 512; want.callback = audio_cb;
    adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!adev) { logmsg("audio: %s\n", SDL_GetError()); return 0; }
    logmsg("audio: open %d Hz, %d ch, %d samples, format %04x\n", have.freq, have.channels, have.samples, have.format);
    SDL_PauseAudioDevice(adev, 0);
    return 1;
}
void plat_audio_lock(void) {
    if (adev) SDL_LockAudioDevice(adev);
}
void plat_audio_unlock(void) { if (adev) SDL_UnlockAudioDevice(adev); }
void plat_midi_send(uint8_t st, uint8_t d1, uint8_t d2) { (void)st; (void)d1; (void)d2; }

static int thread_tramp(void *a) { void **p = a; int (*fn)(void *) = (int (*)(void *))p[0]; void *arg = p[1]; free(p); return fn(arg); }
void plat_thread(int (*fn)(void *), void *arg) { void **p = malloc(2 * sizeof *p); p[0] = (void *)fn; p[1] = arg; SDL_DetachThread(SDL_CreateThread(thread_tramp, "pop2", p)); }
int plat_file_exists(const char *path) { struct stat st; return stat(path, &st) == 0; }
void plat_mkdir(const char *path) { mkdir(path, 0777); }
#ifdef __vita__
void plat_exe_dir(char *out) { snprintf(out, MAX_PATH, "%s", DATA_DIR); }
void plat_temp_dir(char *out) { snprintf(out, MAX_PATH, "%s", DATA_DIR); }
void plat_default_game_dir(char *out) { snprintf(out, MAX_PATH, "%s", DATA_DIR); }
int plat_case_fold(void) { return 0; }
void plat_log_write(const char *line) { (void)line; say("%s", line); }
void plat_fatal_box(const char *title, const char *msg) {
    (void)title; say("FATAL %s\n", msg);
    if (!ren) {
        if (!win && SDL_Init(SDL_INIT_VIDEO) == 0) win = SDL_CreateWindow("Prince of Persia 2", 0, 0, 960, 544, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN);
        if (win) ren = SDL_CreateRenderer(win, -1, 0);
        if (!ren) { say("message dialog: no renderer: %s\n", SDL_GetError()); return; }
    }
    SceMsgDialogUserMessageParam um; memset(&um, 0, sizeof um);
    um.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK; um.msg = (const SceChar8 *)msg;
    SceMsgDialogParam prm; sceMsgDialogParamInit(&prm);
    prm.mode = SCE_MSG_DIALOG_MODE_USER_MSG; prm.userMsgParam = &um;
    int rc = sceMsgDialogInit(&prm);
    if (rc < 0) { say("message dialog: sceMsgDialogInit %08x\n", rc); return; }
    while (sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING) {
        SDL_Event e; while (SDL_PollEvent(&e)) { }
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren); SDL_RenderPresent(ren);
        SDL_Delay(16);
    }
    sceMsgDialogTerm();
}
void plat_install_crash_handler(void) {}
#else
void plat_exe_dir(char *out) {
    char *b = SDL_GetBasePath();
    snprintf(out, MAX_PATH, "%s", b ? b : "./"); if (b) SDL_free(b);
    size_t n = strlen(out); if (n > 1 && out[n - 1] == '/') out[n - 1] = 0;
}
void plat_temp_dir(char *out) { snprintf(out, MAX_PATH, "/tmp"); }
void plat_default_game_dir(char *out) { const char *g = getenv("POP2_GAMEDIR"); if (g) snprintf(out, MAX_PATH, "%s", g); else plat_exe_dir(out); }
int plat_case_fold(void) { return 1; }
void plat_log_write(const char *line) { static FILE *f; if (!f) f = fopen("pop2native.log", "w"); if (f) { fputs(line, f); fflush(f); } }
void plat_fatal_box(const char *title, const char *msg) { if (!hidden_run) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, msg, win); }
void plat_install_crash_handler(void) {}
#endif

int main(int argc, char **argv) {
    char cmd[256] = ""; int n = 0;
    for (int i = 1; i < argc && n < 200; i++) n += snprintf(cmd + n, sizeof cmd - n, "%s%s", i > 1 ? " " : "", argv[i]);
#ifdef __vita__
    diag_open(); mem_log("start"); watchdog_start();
    int rc = game_main(cmd);
    say("POP2-END\n");
    log_flush();
    sceKernelExitProcess(rc);
    return rc;
#else
    return game_main(cmd);
#endif
}
