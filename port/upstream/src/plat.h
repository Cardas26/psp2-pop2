#ifndef PLAT_H
#define PLAT_H
#include <stdint.h>
#include <stddef.h>
#ifndef MAX_PATH
#define MAX_PATH 512
#endif

uint64_t plat_us(void);
uint32_t plat_ms(void);
void plat_sleep_ms(int ms);
void plat_sleep_us(int us);

typedef struct { int up, down, left, right;
                 int b_cross, b_circle, b_square, b_tri, b_l, b_r, b_select, b_start;
                 int rs_up, rs_down, rs_left, rs_right, ls_up, ls_down, ls_left, ls_right; } PlatPad;
int  plat_init(int scale, int fullscreen, int hidden);
void plat_present(const uint8_t *frame, const uint8_t pal[256][3], int filter, int aspect);
int  plat_pump(void);
int  plat_pad_read(PlatPad *p);
int  plat_key_ascii(int scan, int shift, int ctrl);
int  plat_text_input(const char *title, const char *initial, char *out, int n);
void plat_set_title(int filter, int aspect);
void plat_osd(const char *text, int ms);
int  plat_aspect_modes(void);
const char *plat_aspect_name(int i);
int  plat_filter_modes(void);
const char *plat_filter_name(int i);

int  plat_audio_start(int rate, void (*fill)(int16_t *out, int n));
void plat_audio_lock(void);
void plat_audio_unlock(void);
void plat_midi_send(uint8_t st, uint8_t d1, uint8_t d2);

void plat_thread(int (*fn)(void *), void *arg);
int  plat_file_exists(const char *path);
void plat_mkdir(const char *path);
void plat_exe_dir(char *out);
void plat_temp_dir(char *out);
void plat_default_game_dir(char *out);
int  plat_case_fold(void);
void plat_log_write(const char *line);
void plat_fatal_box(const char *title, const char *msg);
void plat_install_crash_handler(void);

void rt_key_event(int scan, int up, int ascii);
void rt_release_keys(void);
void rt_cycle_filter(void);
void rt_cycle_aspect(void);
int  game_main(const char *cmdline);
void logmsg(const char *fmt, ...);
#endif
