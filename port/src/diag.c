#include "diag.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>

__attribute__((used)) const char build_id[] = "POP2-BUILD " POP2_BUILD_ID;
const char *last_call = "start";

#if defined(DIAGNOSTICS)
static SceUID log_fd = -1;
#ifdef DIAGNOSTICS
static volatile uint32_t log_lines;
#endif

#define LOG_RING (64 * 1024)
static char log_ring[LOG_RING];
static volatile uint32_t ring_head, ring_tail; static uint32_t ring_dropped;
static SceKernelLwMutexWork ring_lock __attribute__((aligned(8))), flush_lock __attribute__((aligned(8)));
static int ring_ready;
static void ring_put(const char *s, int n) {
    if (n <= 0) return;
    if (!ring_ready) { if (log_fd >= 0) sceIoWrite(log_fd, s, (SceSize)n); return; }
    sceKernelLockLwMutex(&ring_lock, 1, NULL);
    if (ring_head - ring_tail + (uint32_t)n > LOG_RING) ring_dropped++;
    else {
        uint32_t o = ring_head & (LOG_RING - 1), first = LOG_RING - o;
        if ((uint32_t)n <= first) memcpy(log_ring + o, s, (size_t)n);
        else { memcpy(log_ring + o, s, first); memcpy(log_ring, s + first, (size_t)n - first); }
        ring_head += (uint32_t)n;
    }
    sceKernelUnlockLwMutex(&ring_lock, 1);
}
void log_flush(void) {
    if (!ring_ready || log_fd < 0) return;
    sceKernelLockLwMutex(&flush_lock, 1, NULL);
    sceKernelLockLwMutex(&ring_lock, 1, NULL);
    uint32_t t = ring_tail, h = ring_head, dropped = ring_dropped; ring_dropped = 0;
    sceKernelUnlockLwMutex(&ring_lock, 1);
    while (t != h) {
        uint32_t o = t & (LOG_RING - 1), n = h - t;
        if (o + n > LOG_RING) n = LOG_RING - o;
        sceIoWrite(log_fd, log_ring + o, n); t += n;
    }
    if (dropped) { char b[64]; int n = snprintf(b, sizeof b, "log: %u lines dropped (ring full)\n", dropped); sceIoWrite(log_fd, b, (SceSize)n); }
    sceKernelLockLwMutex(&ring_lock, 1, NULL);
    ring_tail = t;
    sceKernelUnlockLwMutex(&ring_lock, 1);
    sceKernelUnlockLwMutex(&flush_lock, 1);
}
#ifdef DIAGNOSTICS
void say(const char *fmt, ...) {
    char line[640]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    if (n > (int)sizeof line - 1) n = sizeof line - 1;
    if (n > 0) { ring_put(line, n); log_lines++; }
}
#endif
#ifdef DIAGNOSTICS
void diag_alive(void) { log_lines++; }
#endif
void diag_open(void) {
    sceIoMkdir(DATA_DIR, 0777);
    log_fd = sceIoOpen(DATA_DIR "/debug.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    say("%s\n", build_id);
    if (sceKernelCreateLwMutex(&ring_lock, "pop2log", 0, 0, NULL) >= 0 && sceKernelCreateLwMutex(&flush_lock, "pop2logflush", 0, 0, NULL) >= 0) ring_ready = 1;
    say("log: %s\n", ring_ready ? "ring, written once a second by the watchdog thread" : "no mutex, each line written at once");
}
#ifdef DIAGNOSTICS
void mem_log(const char *where) {
    SceKernelFreeMemorySizeInfo mi = { sizeof mi, 0, 0, 0 };
    sceKernelGetFreeMemorySize(&mi);
    say("mem[%s]: user=%d KB cdram=%d KB phycont=%d KB\n", where, mi.size_user / 1024, mi.size_cdram / 1024, mi.size_phycont / 1024);
}

#define WD_HANG_MS 300000
#define WD_HEARTBEAT_MS 60000
static SceUID wd_fd = -1;
static void wd_write(const char *fmt, ...) {
    char buf[200]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (wd_fd >= 0 && n > 0) sceIoWrite(wd_fd, buf, (SceSize)(n > (int)sizeof buf - 1 ? (int)sizeof buf - 1 : n));
}
static int watchdog_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    wd_fd = sceIoOpen(DATA_DIR "/watchdog.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    wd_write("watchdog: running, trap after %d s without a log line\n", WD_HANG_MS / 1000);
    uint32_t last = log_lines; int idle_ms = 0, beat_ms = 0;
    for (;;) {
        sceKernelDelayThread(1000 * 1000);
        uint32_t now = log_lines;
        if (now != last) { last = now; idle_ms = 0; } else idle_ms += 1000;
        if ((beat_ms += 1000) >= WD_HEARTBEAT_MS) { beat_ms = 0; wd_write("watchdog: alive, %u log lines, quiet %d s\n", now, idle_ms / 1000); }
        log_flush();
        if (idle_ms >= WD_HANG_MS) {
            wd_write("watchdog: HANG, %d s without a log line, last_call=%s: trapping for a coredump\n", idle_ms / 1000, last_call);
            sceIoClose(wd_fd);
            __builtin_trap();
        }
    }
    return 0;
}
void watchdog_start(void) {
    SceUID th = sceKernelCreateThread("watchdog", watchdog_thread, 0x48, 0x4000, 0, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    say("watchdog: thread 0x%08x priority 0x48\n", (unsigned)th);
}
#endif
#else
void diag_open(void) {}
#endif

void __real__exit(int code);
void __wrap__exit(int code) {
    say("exit  _exit(%d) from 0x%08x last_call=%s\n", code, (unsigned)(uintptr_t)__builtin_return_address(0), last_call);
    log_flush();
    __real__exit(code);
}
void abort(void) {
    say("exit  abort() from 0x%08x last_call=%s\n", (unsigned)(uintptr_t)__builtin_return_address(0), last_call);
    log_flush();
    sceKernelExitProcess(1);
    for (;;) ;
}
