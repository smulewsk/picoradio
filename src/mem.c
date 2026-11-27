#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <stdbool.h>
#include "FreeRTOS.h"
#include "mem.h"

/* FreeRTOS pool provided here (configAPPLICATION_ALLOCATED_HEAP) so that
 * xmem_free() can tell which heap a block came from. */
uint8_t ucHeap[configTOTAL_HEAP_SIZE] __attribute__((aligned(8)));

extern char __end__, __HeapLimit;          /* C heap range (linker script) */

#define KEEP_FREE 8192                     /* never drain a heap completely */

size_t xmem_c_heap_free(void) {
    struct mallinfo mi = mallinfo();
    size_t total = (size_t)(&__HeapLimit - &__end__);
    size_t arena = (size_t)mi.arena;               /* taken from sbrk so far */
    return (total > arena ? total - arena : 0) + (size_t)mi.fordblks;
}

static bool in_rtos_heap(const void *p) {
    return (const uint8_t *)p >= ucHeap && (const uint8_t *)p < ucHeap + sizeof ucHeap;
}

/* Best fit across the two heaps: use the heap with the LEAST room that
 * still fits the block with margin, so the other heap keeps a large
 * contiguous area for the next big block (e.g. TLS, then the 51 KB AAC
 * SBR state). If that attempt fails (fragmentation), try the other heap. */
void *xmem_alloc(size_t n) {
    size_t rtos = xPortGetFreeHeapSize();
    size_t c = xmem_c_heap_free();
    bool rtos_fits = rtos >= n + KEEP_FREE;
    bool c_fits = c >= n + KEEP_FREE;
    bool rtos_first = rtos_fits && (!c_fits || rtos <= c);

    void *p = NULL;
    if (rtos_first) {
        p = pvPortMalloc(n);
        if (!p) p = malloc(n);
    } else {
        p = malloc(n);
        if (!p && rtos >= n + 1024) p = pvPortMalloc(n);
    }
    return p;
}

void *xmem_calloc(size_t count, size_t size) {
    size_t n = count * size;
    if (size && n / size != count) return NULL;
    void *p = xmem_alloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void xmem_free(void *p) {
    if (!p) return;
    if (in_rtos_heap(p)) vPortFree(p);
    else free(p);
}
