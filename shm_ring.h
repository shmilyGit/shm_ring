/*
 * Derived from DPDK's shm_ring.h
 *
 **************************************************************************
 *
 * @date: 2020-03-15
 * @file  shm_ring.h
 * @brief lockless ring based on DPDK
 *
 ***************************************************************************/

#ifndef _SHM_RING_H_
#define _SHM_RING_H_

/**
 * @file
 * RTE Ring
 *
 * The Ring Manager is a fixed-size queue, implemented as a table of
 * pointers. Head and tail pointers are modified atomically, allowing
 * concurrent access to it. It has the following features:
 *
 * - FIFO (First In First Out)
 * - Maximum size is fixed; the pointers are stored in a table.
 * - Lockless implementation.
 * - Multi- or single-consumer dequeue.
 * - Multi- or single-producer enqueue.
 * - Bulk dequeue.
 * - Bulk enqueue.
 *
 * Note: the ring implementation is not preemptable. A lcore must not
 * be interrupted by another task that uses the same ring.
 *
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <string.h>
#include <stdint.h>
#include <sys/queue.h>
#include <errno.h>
#include <unistd.h>
#include "shm_common.h"

#ifdef __SSE2__
#include <emmintrin.h>
/**
 * PAUSE instruction for tight loops (avoid busy waiting)
 */
static inline void
ue_pause (void)
{
    _mm_pause();
}
#else
static inline void
ue_pause(void) {}
#endif

enum shm_ring_queue_behavior {
	SHM_RING_QUEUE_FIXED = 0, /* Enq/Deq a fixed number of items from a ring */
	SHM_RING_QUEUE_VARIABLE   /* Enq/Deq as many items a possible from ring */
};

#ifdef SHM_RING_DEBUG
/**
 * A structure that stores the ring statistics (per-lcore).
 */
struct shm_ring_debug_stats {
	uint64_t enq_success_bulk; /**< Successful enqueues number. */
	uint64_t enq_success_objs; /**< Objects successfully enqueued. */
	uint64_t enq_quota_bulk;   /**< Successful enqueues above watermark. */
	uint64_t enq_quota_objs;   /**< Objects enqueued above watermark. */
	uint64_t enq_fail_bulk;    /**< Failed enqueues number. */
	uint64_t enq_fail_objs;    /**< Objects that failed to be enqueued. */
	uint64_t deq_success_bulk; /**< Successful dequeues number. */
	uint64_t deq_success_objs; /**< Objects successfully dequeued. */
	uint64_t deq_fail_bulk;    /**< Failed dequeues number. */
	uint64_t deq_fail_objs;    /**< Objects that failed to be dequeued. */
} __shm_cache_aligned;
#endif

/**
 * An RTE ring structure.
 *
 * The producer and the consumer have a head and a tail index. The particularity
 * of these index is that they are not between 0 and size(ring). These indexes
 * are between 0 and 2^32, and we mask their value when we access the ring[]
 * field. Thanks to this assumption, we can do subtractions between 2 index
 * values in a modulo-32bit base: that's why the overflow of the indexes is not
 * a problem.
 */
typedef struct shm_ring {
	char name[SHM_NAMESIZE];    /**< Name of the ring. */
	int flags;                       /**< Flags supplied at creation. */
	int elemt_size;                       /**< Flags supplied at creation. */

	/** Ring producer status. */
	struct prod {
		uint32_t watermark;      /**< Maximum items before EDQUOT. */
		uint32_t sp_enqueue;     /**< True, if single producer. */
		uint32_t size;           /**< Size of ring. */
		uint32_t mask;           /**< Mask (size-1) of ring. */
		volatile uint32_t head;  /**< Producer head. */
		volatile uint32_t tail;  /**< Producer tail. */
	} prod __shm_cache_aligned;

	/** Ring consumer status. */
	struct cons {
		uint32_t sc_dequeue;     /**< True, if single consumer. */
		uint32_t size;           /**< Size of the ring. */
		uint32_t mask;           /**< Mask (size-1) of ring. */
		volatile uint32_t head;  /**< Consumer head. */
		volatile uint32_t tail;  /**< Consumer tail. */
#ifdef SHM_RING_SPLIT_PROD_CONS
	} cons __shm_cache_aligned;
#else
	} cons;
#endif

#ifdef SHM_RING_DEBUG
	struct shm_ring_debug_stats stats[UE_MAX_LCORE];
#endif
    void * mempool;

	void * ring[0] __shm_cache_aligned; /**< Memory space of ring starts here.
	 	 	 	 	 	 	 	 	 	 * not volatile so need to be careful
	 	 	 	 	 	 	 	 	 	 * about compiler re-ordering */
}shm_ring_t;

/* dummy assembly operation to prevent compiler re-ordering of instructions */
#define COMPILER_BARRIER() do { asm volatile("" ::: "memory"); } while(0)

#define RING_F_SP_ENQ 0x0001 /**< The default enqueue is "single-producer". */
#define RING_F_SC_DEQ 0x0002 /**< The default dequeue is "single-consumer". */
#define SHM_RING_QUOT_EXCEED (1 << 31)  /**< Quota exceed for burst ops */
#define SHM_RING_SZ_MASK  (unsigned)(0x0fffffff) /**< Ring size mask */

/**
 * @internal When debug is enabled, store ring statistics.
 * @param r
 *   A pointer to the ring.
 * @param name
 *   The name of the statistics field to increment in the ring.
 * @param n
 *   The number to add to the object-oriented statistics.
 */
#ifdef SHM_RING_DEBUG
#define __ring_stat_ADD(r, name, n) do {		\
		unsigned __lcore_id = ue_lcore_id();	\
		r->stats[__lcore_id].name##_objs += n;	\
		r->stats[__lcore_id].name##_bulk += 1;	\
	} while(0)
#else
#define __ring_stat_ADD(r, name, n) do {} while(0)
#endif

/**
 * Create a new ring named *name* in memory.
 *
 * This function uses ``memzone_reserve()`` to allocate memory. Its size is
 * set to *count*, which must be a power of two. Water marking is
 * disabled by default.
 * Note that the real usable ring size is *count-1* instead of
 * *count*.
 *
 * @param name
 *   The name of the ring.
 * @param count
 *   The size of the ring (must be a power of 2).
 * @param flags
 *   An OR of the following:
 *    - RING_F_SP_ENQ: If this flag is set, the default behavior when
 *      using ``shm_ring_enqueue()`` or ``shm_ring_enqueue_bulk()``
 *      is "single-producer". Otherwise, it is "multi-producers".
 *    - RING_F_SC_DEQ: If this flag is set, the default behavior when
 *      using ``shm_ring_dequeue()`` or ``shm_ring_dequeue_bulk()``
 *      is "single-consumer". Otherwise, it is "multi-consumers".
 * @return
 *   On success, the pointer to the new allocated ring. NULL on error with
 *    ue_errno set appropriately. Possible errno values include:
 *    - E_UE_NO_CONFIG - function could not get pointer to ue_config structure
 *    - E_UE_SECONDARY - function was called from a secondary process instance
 *    - E_UE_NO_TAILQ - no tailq list could be got for the ring list
 *    - EINVAL - count provided is not a power of 2
 *    - ENOSPC - the maximum number of memzones has already been allocated
 *    - EEXIST - a memzone with the same name already exists
 *    - ENOMEM - no appropriate memory area found in which to create memzone
 */
shm_ring_t * shm_ring_create(const char * name, int count);

/**
 * Reset the ring
 */
void shm_ring_reset(shm_ring_t *r);

/**
 * Free the ring buffer
 */
void shm_ring_destroy(shm_ring_t *r);

/**
 * Change the high water mark.
 *
 * If *count* is 0, water marking is disabled. Otherwise, it is set to the
 * *count* value. The *count* value must be greater than 0 and less
 * than the ring size.
 *
 * This function can be called at any time (not necessarily at
 * initialization).
 *
 * @param r
 *   A pointer to the ring structure.
 * @param count
 *   The new water mark value.
 * @return
 *   - 0: Success; water mark changed.
 *   - -EINVAL: Invalid water mark value.
 */
int shm_ring_set_water_mark(shm_ring_t *r, unsigned count);

/**
 * Dump the status of the ring to the console.
 *
 * @param r
 *   A pointer to the ring structure.
 */
void shm_ring_dump(const shm_ring_t *r);

/* the actual enqueue of pointers on the ring. 
 * Placed here since identical code needed in both
 * single and multi producer enqueue functions */
#define ENQUEUE_PTRS() do { \
	const uint32_t size = r->prod.size; \
	uint32_t idx = prod_head & mask; \
	if (likely(idx + n < size)) { \
		for (i = 0; i < (n & ((~(unsigned)0x3))); i+=4, idx+=4) { \
			r->ring[idx] = obj_table[i]; \
			r->ring[idx+1] = obj_table[i+1]; \
			r->ring[idx+2] = obj_table[i+2]; \
			r->ring[idx+3] = obj_table[i+3]; \
		} \
		switch (n & 0x3) { \
			case 3: r->ring[idx++] = obj_table[i++]; \
			case 2: r->ring[idx++] = obj_table[i++]; \
			case 1: r->ring[idx++] = obj_table[i++]; \
		} \
	} else { \
		for (i = 0; idx < size; i++, idx++)\
			r->ring[idx] = obj_table[i]; \
		for (idx = 0; i < n; i++, idx++) \
			r->ring[idx] = obj_table[i]; \
	} \
} while(0)

/* the actual copy of pointers on the ring to obj_table. 
 * Placed here since identical code needed in both
 * single and multi consumer dequeue functions */
#define DEQUEUE_PTRS() do { \
	uint32_t idx = cons_head & mask; \
	const uint32_t size = r->cons.size; \
	if (likely(idx + n < size)) { \
		for (i = 0; i < (n & (~(unsigned)0x3)); i+=4, idx+=4) {\
			obj_table[i] = r->ring[idx]; \
			obj_table[i+1] = r->ring[idx+1]; \
			obj_table[i+2] = r->ring[idx+2]; \
			obj_table[i+3] = r->ring[idx+3]; \
		} \
		switch (n & 0x3) { \
			case 3: obj_table[i++] = r->ring[idx++];\
			case 2: obj_table[i++] = r->ring[idx++];\
			case 1: obj_table[i++] = r->ring[idx++];\
		} \
	} else { \
		for (i = 0; idx < size; i++, idx++) \
			obj_table[i] = r->ring[idx]; \
		for (idx = 0; i < n; i++, idx++) \
			obj_table[i] = r->ring[idx]; \
	} \
} while (0)


/**
 * @internal Enqueue several objects on the ring (multi-producers safe).
 *
 * This function uses a "compare and set" instruction to move the
 * producer index atomically.
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects).
 * @param n
 *   The number of objects to add in the ring from the obj_table.
 * @param behavior
 *   SHM_RING_QUEUE_FIXED:    Enqueue a fixed number of items from a ring
 *   SHM_RING_QUEUE_VARIABLE: Enqueue as many items a possible from ring
 * @return
 *   Depend on the behavior value
 *   if behavior = SHM_RING_QUEUE_FIXED
 *   - 0: Success; objects enqueue.
 *   - -EDQUOT: Quota exceeded. The objects have been enqueued, but the
 *     high water mark is exceeded.
 *   - -ENOBUFS: Not enough room in the ring to enqueue, no object is enqueued.
 *   if behavior = SHM_RING_QUEUE_VARIABLE
 *   - n: Actual number of objects enqueued.
 */
static inline int __attribute__((always_inline))
__shm_ring_mp_do_enqueue(shm_ring_t *r, void *const *obj_table,
			 unsigned n, enum shm_ring_queue_behavior behavior)
{
	uint32_t prod_head, prod_next;
	uint32_t cons_tail, free_entries;
	const unsigned max = n;
	int success;
	unsigned i;
	uint32_t mask = r->prod.mask;
	int ret;

	/* move prod.head atomically */
	do {
		/* Reset n to the initial burst count */
		n = max;

		prod_head = r->prod.head;
		cons_tail = r->cons.tail;
		/* The subtraction is done between two unsigned 32bits value
		 * (the result is always modulo 32 bits even if we have
		 * prod_head > cons_tail). So 'free_entries' is always between 0
		 * and size(ring)-1. */
		free_entries = (mask + cons_tail - prod_head);

		/* check that we have enough room in ring */
		if (unlikely(n > free_entries)) {
			if (behavior == SHM_RING_QUEUE_FIXED) {
				__ring_stat_ADD(r, enq_fail, n);
				return -ENOBUFS;
			}
			else {
				/* No free entry available */
				if (unlikely(free_entries == 0)) {
					__ring_stat_ADD(r, enq_fail, n);
					return 0;
				}

				n = free_entries;
			}
		}

		prod_next = prod_head + n;
		success = shm_atomic32_cmpset(&r->prod.head, prod_head,
					      prod_next);
	} while (unlikely(success == 0));

	/* write entries in ring */
	ENQUEUE_PTRS();
	COMPILER_BARRIER();

	/* if we exceed the watermark */
	if (unlikely(((mask + 1) - free_entries + n) > r->prod.watermark)) {
		ret = (behavior == SHM_RING_QUEUE_FIXED) ? -EDQUOT :
				(int)(n | SHM_RING_QUOT_EXCEED);
		__ring_stat_ADD(r, enq_quota, n);
	}
	else {
		ret = (behavior == SHM_RING_QUEUE_FIXED) ? 0 : n;
		__ring_stat_ADD(r, enq_success, n);
	}

	/*
	 * If there are other enqueues in progress that preceeded us,
	 * we need to wait for them to complete
	 */
	while (unlikely(r->prod.tail != prod_head))
		ue_pause();

	r->prod.tail = prod_next;
	return ret;
}

/**
 * @internal Enqueue several objects on a ring (NOT multi-producers safe).
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects).
 * @param n
 *   The number of objects to add in the ring from the obj_table.
 * @param behavior
 *   SHM_RING_QUEUE_FIXED:    Enqueue a fixed number of items from a ring
 *   SHM_RING_QUEUE_VARIABLE: Enqueue as many items a possible from ring
 * @return
 *   Depend on the behavior value
 *   if behavior = SHM_RING_QUEUE_FIXED
 *   - 0: Success; objects enqueue.
 *   - -EDQUOT: Quota exceeded. The objects have been enqueued, but the
 *     high water mark is exceeded.
 *   - -ENOBUFS: Not enough room in the ring to enqueue, no object is enqueued.
 *   if behavior = SHM_RING_QUEUE_VARIABLE
 *   - n: Actual number of objects enqueued.
 */
static inline int __attribute__((always_inline))
__shm_ring_sp_do_enqueue(shm_ring_t *r, void * const *obj_table,
			 unsigned n, enum shm_ring_queue_behavior behavior)
{
	uint32_t prod_head, cons_tail;
	uint32_t prod_next, free_entries;
	unsigned i;
	uint32_t mask = r->prod.mask;
	int ret;

	prod_head = r->prod.head;
	cons_tail = r->cons.tail;
	/* The subtraction is done between two unsigned 32bits value
	 * (the result is always modulo 32 bits even if we have
	 * prod_head > cons_tail). So 'free_entries' is always between 0
	 * and size(ring)-1. */
	free_entries = mask + cons_tail - prod_head;

	/* check that we have enough room in ring */
	if (unlikely(n > free_entries)) {
		if (behavior == SHM_RING_QUEUE_FIXED) {
			__ring_stat_ADD(r, enq_fail, n);
			return -ENOBUFS;
		}
		else {
			/* No free entry available */
			if (unlikely(free_entries == 0)) {
				__ring_stat_ADD(r, enq_fail, n);
				return 0;
			}

			n = free_entries;
		}
	}

	prod_next = prod_head + n;
	r->prod.head = prod_next;

	/* write entries in ring */
	ENQUEUE_PTRS();
	COMPILER_BARRIER();

	/* if we exceed the watermark */
	if (unlikely(((mask + 1) - free_entries + n) > r->prod.watermark)) {
		ret = (behavior == SHM_RING_QUEUE_FIXED) ? -EDQUOT :
			(int)(n | SHM_RING_QUOT_EXCEED);
		__ring_stat_ADD(r, enq_quota, n);
	}
	else {
		ret = (behavior == SHM_RING_QUEUE_FIXED) ? 0 : n;
		__ring_stat_ADD(r, enq_success, n);
	}

	r->prod.tail = prod_next;
	return ret;
}


/**
 * @internal Dequeue several objects from a ring (multi-consumers safe). When
 * the request objects are more than the available objects, only dequeue the
 * actual number of objects
 *
 * This function uses a "compare and set" instruction to move the
 * consumer index atomically.
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects) that will be filled.
 * @param n
 *   The number of objects to dequeue from the ring to the obj_table.
 * @param behavior
 *   SHM_RING_QUEUE_FIXED:    Dequeue a fixed number of items from a ring
 *   SHM_RING_QUEUE_VARIABLE: Dequeue as many items a possible from ring
 * @return
 *   Depend on the behavior value
 *   if behavior = SHM_RING_QUEUE_FIXED
 *   - 0: Success; objects dequeued.
 *   - -ENOENT: Not enough entries in the ring to dequeue; no object is
 *     dequeued.
 *   if behavior = SHM_RING_QUEUE_VARIABLE
 *   - n: Actual number of objects dequeued.
 */

static inline int __attribute__((always_inline))
__shm_ring_mc_do_dequeue(shm_ring_t *r, void **obj_table,
		 unsigned n, enum shm_ring_queue_behavior behavior)
{
	uint32_t cons_head, prod_tail;
	uint32_t cons_next, entries;
	const unsigned max = n;
	int success;
	unsigned i;
	uint32_t mask = r->prod.mask;

	/* move cons.head atomically */
	do {
		/* Restore n as it may change every loop */
		n = max;

		/* cons_head used to calculate producer */
		cons_head = r->cons.head;
		prod_tail = r->prod.tail;
		/* The subtraction is done between two unsigned 32bits value
		 * (the result is always modulo 32 bits even if we have
		 * cons_head > prod_tail). So 'entries' is always between 0
		 * and size(ring)-1. */
		entries = (prod_tail - cons_head);

		/* Set the actual entries for dequeue */
		if (n > entries) {
			if (behavior == SHM_RING_QUEUE_FIXED) {
				__ring_stat_ADD(r, deq_fail, n);
				return -ENOENT;
			}
			else {
				if (unlikely(entries == 0)){
					__ring_stat_ADD(r, deq_fail, n);
					return 0;
				}

				n = entries;
			}
		}

		cons_next = cons_head + n;
		success = shm_atomic32_cmpset(&r->cons.head, cons_head,
					      cons_next);
	} while (unlikely(success == 0));

	/* copy in table */
	DEQUEUE_PTRS();
	COMPILER_BARRIER();

	/*
	 * If there are other dequeues in progress that preceded us,
	 * we need to wait for them to complete
	 */
	while (unlikely(r->cons.tail != cons_head))
		ue_pause();

	__ring_stat_ADD(r, deq_success, n);
	r->cons.tail = cons_next;

	return behavior == SHM_RING_QUEUE_FIXED ? 0 : n;
}

/**
 * @internal Dequeue several objects from a ring (NOT multi-consumers safe).
 * When the request objects are more than the available objects, only dequeue
 * the actual number of objects
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects) that will be filled.
 * @param n
 *   The number of objects to dequeue from the ring to the obj_table.
 * @param behavior
 *   SHM_RING_QUEUE_FIXED:    Dequeue a fixed number of items from a ring
 *   SHM_RING_QUEUE_VARIABLE: Dequeue as many items a possible from ring
 * @return
 *   Depend on the behavior value
 *   if behavior = SHM_RING_QUEUE_FIXED
 *   - 0: Success; objects dequeued.
 *   - -ENOENT: Not enough entries in the ring to dequeue; no object is
 *     dequeued.
 *   if behavior = SHM_RING_QUEUE_VARIABLE
 *   - n: Actual number of objects dequeued.
 */
static inline int __attribute__((always_inline))
__shm_ring_sc_do_dequeue(shm_ring_t *r, void **obj_table,
		 unsigned n, enum shm_ring_queue_behavior behavior)
{
	uint32_t cons_head, prod_tail;
	uint32_t cons_next, entries;
	unsigned i;
	uint32_t mask = r->prod.mask;

	cons_head = r->cons.head;
	prod_tail = r->prod.tail;
	/* The subtraction is done between two unsigned 32bits value
	 * (the result is always modulo 32 bits even if we have
	 * cons_head > prod_tail). So 'entries' is always between 0
	 * and size(ring)-1. */
	entries = prod_tail - cons_head;

	if (n > entries) {
		if (behavior == SHM_RING_QUEUE_FIXED) {
			__ring_stat_ADD(r, deq_fail, n);
			return -ENOENT;
		}
		else {
			if (unlikely(entries == 0)){
				__ring_stat_ADD(r, deq_fail, n);
				return 0;
			}

			n = entries;
		}
	}

	cons_next = cons_head + n;
	r->cons.head = cons_next;

	/* copy in table */
	DEQUEUE_PTRS();
	COMPILER_BARRIER();

	__ring_stat_ADD(r, deq_success, n);
	r->cons.tail = cons_next;
	return behavior == SHM_RING_QUEUE_FIXED ? 0 : n;
}

/**
 * Enqueue several objects on the ring (multi-producers safe).
 *
 * This function uses a "compare and set" instruction to move the
 * producer index atomically.
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects).
 * @param n
 *   The number of objects to add in the ring from the obj_table.
 * @return
 *   - 0: Success; objects enqueue.
 *   - -EDQUOT: Quota exceeded. The objects have been enqueued, but the
 *     high water mark is exceeded.
 *   - -ENOBUFS: Not enough room in the ring to enqueue, no object is enqueued.
 */
static inline int __attribute__((always_inline))
shm_ring_mp_enqueue_bulk(shm_ring_t *r, void * const *obj_table, unsigned n)
{
	return __shm_ring_mp_do_enqueue(r, obj_table, n, SHM_RING_QUEUE_FIXED);
}

/**
 * Enqueue several objects on a ring (NOT multi-producers safe).
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects).
 * @param n
 *   The number of objects to add in the ring from the obj_table.
 * @return
 *   - 0: Success; objects enqueued.
 *   - -EDQUOT: Quota exceeded. The objects have been enqueued, but the
 *     high water mark is exceeded.
 *   - -ENOBUFS: Not enough room in the ring to enqueue; no object is enqueued.
 */
static inline int __attribute__((always_inline))
shm_ring_sp_enqueue_bulk(shm_ring_t *r, void * const *obj_table,
			 unsigned n)
{
	return __shm_ring_sp_do_enqueue(r, obj_table, n, SHM_RING_QUEUE_FIXED);
}

/**
 * Enqueue several objects on a ring.
 *
 * This function calls the multi-producer or the single-producer
 * version depending on the default behavior that was specified at
 * ring creation time (see flags).
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects).
 * @param n
 *   The number of objects to add in the ring from the obj_table.
 * @return
 *   - 0: Success; objects enqueued.
 *   - -EDQUOT: Quota exceeded. The objects have been enqueued, but the
 *     high water mark is exceeded.
 *   - -ENOBUFS: Not enough room in the ring to enqueue; no object is enqueued.
 */
static inline int __attribute__((always_inline))
shm_ring_enqueue_bulk(shm_ring_t *r, void * const *obj_table,
		      unsigned n)
{
	if (r->prod.sp_enqueue)
		return shm_ring_sp_enqueue_bulk(r, obj_table, n);
	else
		return shm_ring_mp_enqueue_bulk(r, obj_table, n);
}

/**
 * Enqueue one object on a ring (multi-producers safe).
 *
 * This function uses a "compare and set" instruction to move the
 * producer index atomically.
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj
 *   A pointer to the object to be added.
 * @return
 *   - 0: Success; objects enqueued.
 *   - -EDQUOT: Quota exceeded. The objects have been enqueued, but the
 *     high water mark is exceeded.
 *   - -ENOBUFS: Not enough room in the ring to enqueue; no object is enqueued.
 */
static inline int __attribute__((always_inline))
shm_ring_mp_enqueue(shm_ring_t *r, void *obj)
{
	return shm_ring_mp_enqueue_bulk(r, &obj, 1);
}

/**
 * Enqueue one object on a ring (NOT multi-producers safe).
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj
 *   A pointer to the object to be added.
 * @return
 *   - 0: Success; objects enqueued.
 *   - -EDQUOT: Quota exceeded. The objects have been enqueued, but the
 *     high water mark is exceeded.
 *   - -ENOBUFS: Not enough room in the ring to enqueue; no object is enqueued.
 */
static inline int __attribute__((always_inline))
shm_ring_sp_enqueue(shm_ring_t *r, void *obj)
{
	return shm_ring_sp_enqueue_bulk(r, &obj, 1);
}

/**
 * Enqueue one object on a ring.
 *
 * This function calls the multi-producer or the single-producer
 * version, depending on the default behaviour that was specified at
 * ring creation time (see flags).
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj
 *   A pointer to the object to be added.
 * @return
 *   - 0: Success; objects enqueued.
 *   - -EDQUOT: Quota exceeded. The objects have been enqueued, but the
 *     high water mark is exceeded.
 *   - -ENOBUFS: Not enough room in the ring to enqueue; no object is enqueued.
 */
static inline int __attribute__((always_inline))
shm_ring_enqueue(shm_ring_t *r, void *obj)
{
	if (r->prod.sp_enqueue)
		return shm_ring_sp_enqueue(r, obj);
	else
		return shm_ring_mp_enqueue(r, obj);
}

/**
 * Dequeue several objects from a ring (multi-consumers safe).
 *
 * This function uses a "compare and set" instruction to move the
 * consumer index atomically.
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects) that will be filled.
 * @param n
 *   The number of objects to dequeue from the ring to the obj_table.
 * @return
 *   - 0: Success; objects dequeued.
 *   - -ENOENT: Not enough entries in the ring to dequeue; no object is
 *     dequeued.
 */
static inline int __attribute__((always_inline))
shm_ring_mc_dequeue_bulk(shm_ring_t *r, void **obj_table, unsigned n)
{
	return __shm_ring_mc_do_dequeue(r, obj_table, n, SHM_RING_QUEUE_FIXED);
}

/**
 * Dequeue several objects from a ring (NOT multi-consumers safe).
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects) that will be filled.
 * @param n
 *   The number of objects to dequeue from the ring to the obj_table,
 *   must be strictly positive.
 * @return
 *   - 0: Success; objects dequeued.
 *   - -ENOENT: Not enough entries in the ring to dequeue; no object is
 *     dequeued.
 */
static inline int __attribute__((always_inline))
shm_ring_sc_dequeue_bulk(shm_ring_t *r, void **obj_table, unsigned n)
{
	return __shm_ring_sc_do_dequeue(r, obj_table, n, SHM_RING_QUEUE_FIXED);
}

/**
 * Dequeue several objects from a ring.
 *
 * This function calls the multi-consumers or the single-consumer
 * version, depending on the default behaviour that was specified at
 * ring creation time (see flags).
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects) that will be filled.
 * @param n
 *   The number of objects to dequeue from the ring to the obj_table.
 * @return
 *   - 0: Success; objects dequeued.
 *   - -ENOENT: Not enough entries in the ring to dequeue, no object is
 *     dequeued.
 */
static inline int __attribute__((always_inline))
shm_ring_dequeue_bulk(shm_ring_t *r, void **obj_table, unsigned n)
{
	if (r->cons.sc_dequeue)
		return shm_ring_sc_dequeue_bulk(r, obj_table, n);
	else
		return shm_ring_mc_dequeue_bulk(r, obj_table, n);
}

/**
 * Dequeue one object from a ring (multi-consumers safe).
 *
 * This function uses a "compare and set" instruction to move the
 * consumer index atomically.
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_p
 *   A pointer to a void * pointer (object) that will be filled.
 * @return
 *   - 0: Success; objects dequeued.
 *   - -ENOENT: Not enough entries in the ring to dequeue; no object is
 *     dequeued.
 */
static inline int __attribute__((always_inline))
shm_ring_mc_dequeue(shm_ring_t *r, void **obj_p)
{
	return shm_ring_mc_dequeue_bulk(r, obj_p, 1);
}

/**
 * Dequeue one object from a ring (NOT multi-consumers safe).
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_p
 *   A pointer to a void * pointer (object) that will be filled.
 * @return
 *   - 0: Success; objects dequeued.
 *   - -ENOENT: Not enough entries in the ring to dequeue, no object is
 *     dequeued.
 */
static inline int __attribute__((always_inline))
shm_ring_sc_dequeue(shm_ring_t *r, void **obj_p)
{
	return shm_ring_sc_dequeue_bulk(r, obj_p, 1);
}

/**
 * Dequeue one object from a ring.
 *
 * This function calls the multi-consumers or the single-consumer
 * version depending on the default behaviour that was specified at
 * ring creation time (see flags).
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_p
 *   A pointer to a void * pointer (object) that will be filled.
 * @return
 *   - 0: Success, objects dequeued.
 *   - -ENOENT: Not enough entries in the ring to dequeue, no object is
 *     dequeued.
 */
static inline int __attribute__((always_inline))
shm_ring_dequeue(shm_ring_t *r, void **obj_p)
{
	if (r->cons.sc_dequeue)
		return shm_ring_sc_dequeue(r, obj_p);
	else
		return shm_ring_mc_dequeue(r, obj_p);
}

/**
 * Test if a ring is full.
 *
 * @param r
 *   A pointer to the ring structure.
 * @return
 *   - 1: The ring is full.
 *   - 0: The ring is not full.
 */
static inline int
shm_ring_full(const shm_ring_t *r)
{
	uint32_t prod_tail = r->prod.tail;
	uint32_t cons_tail = r->cons.tail;
	return (((cons_tail - prod_tail - 1) & r->prod.mask) == 0);
}

/**
 * Test if a ring is empty.
 *
 * @param r
 *   A pointer to the ring structure.
 * @return
 *   - 1: The ring is empty.
 *   - 0: The ring is not empty.
 */
static inline int
shm_ring_empty(const shm_ring_t *r)
{
	uint32_t prod_tail = r->prod.tail;
	uint32_t cons_tail = r->cons.tail;
	return !!(cons_tail == prod_tail);
}

/**
 * Return the number of entries in a ring.
 *
 * @param r
 *   A pointer to the ring structure.
 * @return
 *   The number of entries in the ring.
 */
static inline unsigned
shm_ring_count(const shm_ring_t *r)
{
	uint32_t prod_tail = r->prod.tail;
	uint32_t cons_tail = r->cons.tail;
	return ((prod_tail - cons_tail) & r->prod.mask);
}

/**
 * Return the number of free entries in a ring.
 *
 * @param r
 *   A pointer to the ring structure.
 * @return
 *   The number of free entries in the ring.
 */
static inline unsigned
shm_ring_free_count(const shm_ring_t *r)
{
	uint32_t prod_tail = r->prod.tail;
	uint32_t cons_tail = r->cons.tail;
	return ((cons_tail - prod_tail - 1) & r->prod.mask);
}

/**
 * Dump the status of all rings on the console
 */
void shm_ring_list_dump(void);

/**
 * Search a ring from its name
 *
 * @param name
 *   The name of the ring.
 * @return
 *   The pointer to the ring matching the name, or NULL if not found,
 *   with ue_errno set appropriately. Possible ue_errno values include:
 *    - ENOENT - required entry not available to return.
 */
shm_ring_t *shm_ring_lookup(const char *name);

/**
 * Enqueue several objects on the ring (multi-producers safe).
 *
 * This function uses a "compare and set" instruction to move the
 * producer index atomically.
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects).
 * @param n
 *   The number of objects to add in the ring from the obj_table.
 * @return
 *   - n: Actual number of objects enqueued.
 */
static inline int __attribute__((always_inline))
shm_ring_mp_enqueue_burst(shm_ring_t *r, void * const *obj_table,
			 unsigned n)
{
	return __shm_ring_mp_do_enqueue(r, obj_table, n, SHM_RING_QUEUE_VARIABLE);
}

/**
 * Enqueue several objects on a ring (NOT multi-producers safe).
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects).
 * @param n
 *   The number of objects to add in the ring from the obj_table.
 * @return
 *   - n: Actual number of objects enqueued.
 */
static inline int __attribute__((always_inline))
shm_ring_sp_enqueue_burst(shm_ring_t *r, void * const *obj_table,
			 unsigned n)
{
	return __shm_ring_sp_do_enqueue(r, obj_table, n, SHM_RING_QUEUE_VARIABLE);
}

/**
 * Enqueue several objects on a ring.
 *
 * This function calls the multi-producer or the single-producer
 * version depending on the default behavior that was specified at
 * ring creation time (see flags).
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects).
 * @param n
 *   The number of objects to add in the ring from the obj_table.
 * @return
 *   - n: Actual number of objects enqueued.
 */
static inline int __attribute__((always_inline))
shm_ring_enqueue_burst(shm_ring_t *r, void * const *obj_table,
		      unsigned n)
{
	if (r->prod.sp_enqueue)
		return 	shm_ring_sp_enqueue_burst(r, obj_table, n);
	else
		return 	shm_ring_mp_enqueue_burst(r, obj_table, n);
}

/**
 * Dequeue several objects from a ring (multi-consumers safe). When the request
 * objects are more than the available objects, only dequeue the actual number
 * of objects
 *
 * This function uses a "compare and set" instruction to move the
 * consumer index atomically.
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects) that will be filled.
 * @param n
 *   The number of objects to dequeue from the ring to the obj_table.
 * @return
 *   - n: Actual number of objects dequeued, 0 if ring is empty
 */
static inline int __attribute__((always_inline))
shm_ring_mc_dequeue_burst(shm_ring_t *r, void **obj_table, unsigned n)
{
	return __shm_ring_mc_do_dequeue(r, obj_table, n, SHM_RING_QUEUE_VARIABLE);
}

/**
 * Dequeue several objects from a ring (NOT multi-consumers safe).When the
 * request objects are more than the available objects, only dequeue the
 * actual number of objects
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects) that will be filled.
 * @param n
 *   The number of objects to dequeue from the ring to the obj_table.
 * @return
 *   - n: Actual number of objects dequeued, 0 if ring is empty
 */
static inline int __attribute__((always_inline))
shm_ring_sc_dequeue_burst(shm_ring_t *r, void **obj_table, unsigned n)
{
	return __shm_ring_sc_do_dequeue(r, obj_table, n, SHM_RING_QUEUE_VARIABLE);
}

/**
 * Dequeue multiple objects from a ring up to a maximum number.
 *
 * This function calls the multi-consumers or the single-consumer
 * version, depending on the default behaviour that was specified at
 * ring creation time (see flags).
 *
 * @param r
 *   A pointer to the ring structure.
 * @param obj_table
 *   A pointer to a table of void * pointers (objects) that will be filled.
 * @param n
 *   The number of objects to dequeue from the ring to the obj_table.
 * @return
 *   - Number of objects dequeued, or a negative error code on error
 */
static inline int __attribute__((always_inline))
shm_ring_dequeue_burst(shm_ring_t *r, void **obj_table, unsigned n)
{
	if (r->cons.sc_dequeue)
		return shm_ring_sc_dequeue_burst(r, obj_table, n);
	else
		return shm_ring_mc_dequeue_burst(r, obj_table, n);
}

#ifdef __cplusplus
}
#endif

#endif /* _SHM_RING_H_ */
