#ifndef _DEMO_RING_UTIL_
#define _DEMO_RING_UTIL_
#include <unistd.h>
#include <stdint.h>
#include <sys/sysinfo.h>

#define SHM_POOL_BASE_ADDR   0x6ff000000000
#define SHM_ELEMT_SIZE       1024
#define SHM_ELEMT_COUNT      1 << 13

#ifndef SHM_NAMESIZE
#define SHM_NAMESIZE 32 /**< The maximum length of a ring name. */
#endif

#ifndef MP_CACHE_LINE_SIZE
#define MP_CACHE_LINE_SIZE 64
#define MP_CACHE_LINE_MASK (MP_CACHE_LINE_SIZE-1)
#define __mp_cache_aligned __attribute__((__aligned__(MP_CACHE_LINE_SIZE)))
#endif

#ifndef likely
#define likely(x)  __builtin_expect((x),1)
#endif

#ifndef unlikely
#define unlikely(x)  __builtin_expect((x),0)
#endif

#ifndef MPLOCKED
#define MPLOCKED        "lock ; "       /**< Insert MP lock prefix. */
#endif

#define SHM_MEMPOOL_FORCE_INTRINSICS 0

static inline int
shm_mempool_atomic32_cmpset(volatile uint32_t *dst, uint32_t exp, uint32_t src)
{
#ifndef SHM_MEMPOOL_FORCE_INTRINSICS
    uint8_t res;

    asm volatile(
            MPLOCKED
            "cmpxchgl %[src], %[dst];"
            "sete %[res];"
            : [res] "=a" (res),     /* output */
              [dst] "=m" (*dst)
            : [src] "r" (src),      /* input */
              "a" (exp),
              "m" (*dst)
            : "memory");            /* no-clobber list */
    return res;
#else
    return __sync_bool_compare_and_swap(dst, exp, src);
#endif
}

typedef struct shm_mempool {
	uint32_t size;            /**< 内存池总大小. */
	uint32_t mask;            /**< 循环用的掩码. */
	uint32_t nb_elemts;       /**< 内存池内元素总个数. */
	uint32_t elemt_size;      /**< 内存池内每个元素的大小. */

	volatile uint32_t pos;    /**< 内存池内元素当前位置. */
	volatile uint32_t total_used;      /**< 内存池内已使用元素的个数. */

    char name[SHM_NAMESIZE];  /** mmap 映射的文件路径*/

	void * data[0] __mp_cache_aligned;
}shm_mempool_t;

typedef struct shm_elemt {
    uint32_t pid;
    uint32_t seq;
    uint32_t status;
    uint32_t used;
    uint32_t data_len;
    uint32_t padding;
    void * mempool;
    void * data[0];
}shm_elemt_t;

shm_mempool_t * shm_mempool_create(char * name, int elemt_size, int nb_elemts);
shm_mempool_t * shm_mempool_lookup(char * name);
shm_elemt_t * shm_mempool_alloc(shm_mempool_t * mempool);
void shm_mempool_free(shm_elemt_t * elemt);
void shm_mempool_destroy(shm_mempool_t * mempool);


#endif
