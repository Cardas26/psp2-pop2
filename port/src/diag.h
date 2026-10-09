#ifndef POP2_DIAG_H
#define POP2_DIAG_H
#define DATA_DIR "ux0:data/pop2-recomp"
extern const char *last_call;
void diag_open(void);
#if defined(DIAGNOSTICS)
void log_flush(void);
void watchdog_start(void);
#else
#define log_flush() ((void)0)
#define watchdog_start() ((void)0)
#endif
#ifdef DIAGNOSTICS
void say(const char *fmt, ...);
void mem_log(const char *where);
void diag_alive(void);
#else
#define say(...) ((void)0)
#define mem_log(where) ((void)0)
#define diag_alive() ((void)0)
#endif
#endif
