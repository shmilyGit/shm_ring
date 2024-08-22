#ifndef _SHM_COMMON_H_
#define _SHM_COMMON_H_

#ifdef __cplusplus
extern "C" {
#endif

/** 内存池和环形队列的基础地址 **/
#define SHM_RING_BASE_ADDR   0x6ff000000000
#define SHM_POOL_BASE_ADDR   0x6ee000000000

#define SHM_NAMESIZE        32
#define SHM_ELEMT_SIZE      1024
#define SHM_ELEMT_COUNT     (1 << 13)

#ifndef SHM_CACHE_LINE_SIZE
#define SHM_CACHE_LINE_SIZE     64
#define SHM_CACHE_LINE_MASK (SHM_CACHE_LINE_SIZE-1)
#define __shm_cache_aligned __attribute__((__aligned__(SHM_CACHE_LINE_SIZE)))
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

#define SHM_FORCE_INTRINSICS 0



/**
 * Check if a branch is likely to be taken.
 *
 * This compiler builtin allows the developer to indicate if a branch is
 * likely to be taken. Example:
 *
 *   if (likely(x > 1))
 *      do_stuff();
 *
 */
#ifndef likely
#define likely(x)  __builtin_expect((x),1)
#endif /* likely */

/**
 * Check if a branch is unlikely to be taken.
 *
 * This compiler builtin allows the developer to indicate if a branch is
 * unlikely to be taken. Example:
 *
 *   if (unlikely(x < 1))
 *      do_stuff();
 *
 */
#ifndef unlikely
#define unlikely(x)  __builtin_expect((x),0)
#endif /* unlikely */

/**
 * Atomic compare and set.
 *
 * (atomic) equivalent to:
 *   if (*dst == exp)
 *     *dst = src (all 32-bit words)
 *
 * @param dst
 *   The destination location into which the value will be written.
 * @param exp
 *   The expected value.
 * @param src
 *   The new value.
 * @return
 *   Non-zero on success; 0 on failure.
 */
static inline int
shm_atomic32_cmpset(volatile uint32_t *dst, uint32_t exp, uint32_t src)
{
#ifndef SHM_FORCE_INTRINSICS
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
















#ifdef __cplusplus
}
#endif

#endif /* _SHM_COMMON_H_ */
