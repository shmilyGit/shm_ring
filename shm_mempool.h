#ifndef _SHM_MEMPOOL_UTIL_
#define _SHM_MEMPOOL_UTIL_
#include <unistd.h>
#include <stdint.h>
#include <sys/sysinfo.h>
#include "shm_common.h"

typedef struct shm_mempool {
	uint32_t size;            /**< 内存池总大小. */
	uint32_t mask;            /**< 循环用的掩码. */
	uint32_t nb_elemts;       /**< 内存池内元素总个数. */
	uint32_t elemt_size;      /**< 内存池内每个元素的大小. */

	volatile uint32_t pos;    /**< 内存池内元素当前位置. */
	volatile uint32_t total_used;      /**< 内存池内已使用元素的个数. */

    char name[SHM_NAMESIZE];  /** mmap 映射的文件路径*/

	void * data[0] __shm_cache_aligned;
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
