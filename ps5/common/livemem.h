#ifndef PSCLOUD_LIVEMEM_H
#define PSCLOUD_LIVEMEM_H
#include <stddef.h>
/* Live mode: find and change a number in the running game's memory, the way a memory
 * search tool does. Scans run in the background; only addresses found by the user's own
 * search can be written. Types: int32, int16, uint8, float, double. */
int pscloud_live_scan(const char *type,double value,char *message,size_t max);   /* new search */
int pscloud_live_next(double value,char *message,size_t max);                    /* keep places now holding value */
void pscloud_live_reset(void);
void pscloud_live_state_json(char *out,size_t max);
void pscloud_live_list_json(char *out,size_t max,int limit);
int pscloud_live_set(unsigned long long address,double value,char *message,size_t max);
/* Named groups retain only addresses from a search in the current process session. */
int pscloud_live_pin(const char *addresses,const char *name,char *message,size_t max);
int pscloud_live_edit(const char *addresses,unsigned id,const char *action,double value,char *message,size_t max);
void pscloud_live_pins_json(char *out,size_t max);
void pscloud_live_shutdown(void);
int pscloud_live_game(char title[16]);
int pscloud_live_locator(unsigned id,const char *version,char *out,size_t max);
int pscloud_live_resolve(const char *title,const char *type,const char *patterns,char *message,size_t max);
/* Pointer paths: find them for a saved value (background, phase "chains"; Retain then includes
 * them), and hand saved paths to the next relocation ("a/b/c;d/e,f": rules by comma). */
int pscloud_live_chains(unsigned id,char *message,size_t max);
int pscloud_live_use_chains(const char *text);
unsigned pscloud_live_sequence(void);
/* File that records each memory region before it is read (empty path: off). */
void pscloud_live_set_trace(const char *path);
/* Freeze is refused unless switched on (off in the dashboard; see livemem.c). */
void pscloud_live_allow_freeze(int on);
/* Read-only diagnostic: frame counter candidates in the game's own data, as JSON. */
int pscloud_live_frame_probe(char *out,size_t max);
/* FPS readout: start chooses the frame counter with a probe; json reads it (about once a second). */
int pscloud_live_fps_start(char *out,size_t max);
void pscloud_live_fps_json(char *out,size_t max);
void pscloud_live_fps_stop(void);
#endif
