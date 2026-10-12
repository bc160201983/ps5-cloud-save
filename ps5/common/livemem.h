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
#endif
