#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/sysinfo.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include "shm_ring.h"

static uintptr_t align_address(uintptr_t addr)
{
    long page_size = sysconf(_SC_PAGESIZE); 

    if (page_size == -1) {
        page_size = 4096;
    }

    return (addr + page_size - 1) & ~(page_size - 1);
}

/*
 * change the high water mark. If *count* is 0, water marking is
 * disabled
 */
int
shm_ring_set_water_mark(shm_ring_t *r, unsigned count)
{
	if (count >= r->prod.size)
		return -EINVAL;

	/* if count is 0, disable the watermarking */
	if (count == 0)
		count = r->prod.size;

	r->prod.watermark = count;
	return 0;
}

/* dump the status of the ring on the console */
void
shm_ring_dump(const shm_ring_t *r)
{
#ifdef SHM_RING_DEBUG
	shm_ring_t_debug_stats sum;
	unsigned lcore_id;
#endif

	printf("ring <%s>@%p\n", r->name, r);
	printf("  flags=%x\n", r->flags);
	printf("  size=%"PRIu32"\n", r->prod.size);
	printf("  ct=%"PRIu32"\n", r->cons.tail);
	printf("  ch=%"PRIu32"\n", r->cons.head);
	printf("  pt=%"PRIu32"\n", r->prod.tail);
	printf("  ph=%"PRIu32"\n", r->prod.head);
	printf("  used=%u\n", shm_ring_count(r));
	printf("  avail=%u\n", shm_ring_free_count(r));
	if (r->prod.watermark == r->prod.size)
		printf("  watermark=0\n");
	else
		printf("  watermark=%"PRIu32"\n", r->prod.watermark);

	/* sum and dump statistics */
#ifdef SHM_RING_DEBUG
	memset(&sum, 0, sizeof(sum));
	for (lcore_id = 0; lcore_id < UE_MAX_LCORE; lcore_id++) {
		sum.enq_success_bulk += r->stats[lcore_id].enq_success_bulk;
		sum.enq_success_objs += r->stats[lcore_id].enq_success_objs;
		sum.enq_quota_bulk += r->stats[lcore_id].enq_quota_bulk;
		sum.enq_quota_objs += r->stats[lcore_id].enq_quota_objs;
		sum.enq_fail_bulk += r->stats[lcore_id].enq_fail_bulk;
		sum.enq_fail_objs += r->stats[lcore_id].enq_fail_objs;
		sum.deq_success_bulk += r->stats[lcore_id].deq_success_bulk;
		sum.deq_success_objs += r->stats[lcore_id].deq_success_objs;
		sum.deq_fail_bulk += r->stats[lcore_id].deq_fail_bulk;
		sum.deq_fail_objs += r->stats[lcore_id].deq_fail_objs;
	}
	printf("  size=%"PRIu32"\n", r->prod.size);
	printf("  enq_success_bulk=%"PRIu64"\n", sum.enq_success_bulk);
	printf("  enq_success_objs=%"PRIu64"\n", sum.enq_success_objs);
	printf("  enq_quota_bulk=%"PRIu64"\n", sum.enq_quota_bulk);
	printf("  enq_quota_objs=%"PRIu64"\n", sum.enq_quota_objs);
	printf("  enq_fail_bulk=%"PRIu64"\n", sum.enq_fail_bulk);
	printf("  enq_fail_objs=%"PRIu64"\n", sum.enq_fail_objs);
	printf("  deq_success_bulk=%"PRIu64"\n", sum.deq_success_bulk);
	printf("  deq_success_objs=%"PRIu64"\n", sum.deq_success_objs);
	printf("  deq_fail_bulk=%"PRIu64"\n", sum.deq_fail_bulk);
	printf("  deq_fail_objs=%"PRIu64"\n", sum.deq_fail_objs);
#else
	printf("  no statistics available\n");
#endif
}

void
shm_ring_reset(shm_ring_t *r)
{
	r->prod.head = r->cons.head = 0;
	r->prod.tail = r->cons.tail = 0;
}

/*
 * alloc and init ring
 *
 * @return 
 *   0 is success, less than 0 is failed
 */
shm_ring_t* shm_ring_init(const char * name, int elemt_size, int count)
{
    int fd = 0;
	size_t flags = 0;
	size_t ring_size = 0;
    uintptr_t map_addr = 0;

    shm_ring_t *ring = NULL;

	fd = open(name, O_RDWR |O_CREAT, 0666);
	if(fd == -1){
		return NULL;
	}

	ring_size = count * elemt_size + sizeof(shm_ring_t);
	if(ftruncate(fd, ring_size) == -1){
		close(fd);
		return NULL;
	}

    map_addr =  align_address (SHM_RING_BASE_ADDR + ring_size);
	ring = mmap((void *)map_addr, ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd , 0);
	if(ring == MAP_FAILED ){
		close(fd);
		return NULL;
	}

	if (ring != NULL) {
		/* init the ring structure */
		memset(ring, 0, sizeof(*ring));
		snprintf(ring->name, sizeof(ring->name), "%s", name);
		ring->flags = flags;
		ring->elemt_size = elemt_size;
		ring->prod.watermark = count;
		ring->prod.sp_enqueue = !!(flags & RING_F_SP_ENQ);
		ring->cons.sc_dequeue = !!(flags & RING_F_SC_DEQ);
		ring->prod.size = ring->cons.size = count;
		ring->prod.mask = ring->cons.mask = count - 1;
		ring->prod.head = ring->cons.head = 0;
		ring->prod.tail = ring->cons.tail = 0;
	} else {
        return NULL;
	}

    //printf ("Create ===> count: %d, ring_size: %ld\n", count, ring_size - sizeof(shm_ring_t));
    return ring;
}

shm_ring_t* shm_ring_lookup(const char *name)
{
    int fd = 0;
	size_t ring_size;
    uintptr_t map_addr = 0;

    shm_ring_t * ring = NULL;
    shm_ring_t * ring_tmp = NULL;

    fd = open(name, O_RDWR, 0666);
    if(fd == -1) {
		return NULL;
	}

    ring_tmp = (shm_ring_t*)mmap(NULL, sizeof(shm_ring_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd , 0);
    if(ring_tmp == MAP_FAILED){
		close(fd);
		return NULL;
	}

	ring_size = ring_tmp->elemt_size * ring_tmp->prod.size + sizeof(shm_ring_t);

    map_addr =  align_address (SHM_RING_BASE_ADDR + ring_size);
    ring = (shm_ring_t*)mmap((void *)map_addr, ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd , 0);

    if(ring == MAP_FAILED ){
		close(fd);
		return NULL;
	}

    close(fd);
    //printf ("LookUp ===> nb_elemts: %d, ring_size: %ld\n", ring->prod.size, ring_size - sizeof(shm_ring_t));

    return ring;
}

shm_ring_t* shm_ring_create(const char * name, int count){
    if (access(name, F_OK)) {
        return shm_ring_init(name, sizeof(void *), count);
    }
    else {
        return shm_ring_lookup(name);
    }
}

void shm_ring_destroy(shm_ring_t *r){
    if (r) {
        ;//munmap(ring, );
    }
}
