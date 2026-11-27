#pragma once
#include <stddef.h>

/* The RP2350 RAM left over is split between two heaps: the FreeRTOS pool
 * (pvPortMalloc) and the C heap (malloc). Big, short-lived users (TLS,
 * AAC decoder) allocate through here so a block lands in whichever heap
 * has more room, instead of failing because one heap alone is too small. */

void  *xmem_alloc(size_t n);
void  *xmem_calloc(size_t count, size_t size);
void   xmem_free(void *p);
size_t xmem_c_heap_free(void);      /* approximate free bytes in the C heap */
