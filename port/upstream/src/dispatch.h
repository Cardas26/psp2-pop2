#ifndef DISPATCH_H
#define DISPATCH_H
#include <stdint.h>
typedef void (*Fn)(void);
typedef struct { uint16_t seg, off; Fn fn; } FnEntry;
typedef struct { const char *name; int ovl_id; uint16_t seg; int size; int fileoff; int memsize; int flags;
                 const uint8_t *fp; int fpmid;
                 const FnEntry *tab; } SpaceTab;
extern const int fp_len;
#define OVL_COMPRESSED 0x20
extern const SpaceTab space_tabs[];
extern const char pop2_version[];
extern const long pop2_exe_size;
extern const uint32_t pop2_fp_off;
extern const uint8_t pop2_fp[2];
typedef struct { uint32_t off; uint8_t len; uint8_t old[4], new_[4]; } ExePatch;
extern const ExePatch pop2_patches[];
typedef struct { const char *name, *alt[2]; uint32_t size; uint8_t sha256[32]; } GameFile;
extern const GameFile pop2_files[];
typedef struct { uint8_t sha256[4]; const char *what; } OtherExe;
extern const OtherExe pop2_other_exes[];
#endif
