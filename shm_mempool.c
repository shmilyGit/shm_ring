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
#include <errno.h>
#include "shm_mempool.h"

static uintptr_t align_address(uintptr_t addr)
{
    long page_size = sysconf(_SC_PAGESIZE); 

    if (page_size == -1) {
        page_size = 4096;
    }

    return (addr + page_size - 1) & ~(page_size - 1);
}


/**
*功能: 初始化内存池
*参数: name, mmap 映射的文件名
*参数: elemt_size, 内存池中每个元素的大小
*参数: nb_elemts, 内存池中元素的个数
*返回值: 成功:内存池头指针 失败:NULL
*/
shm_mempool_t* shm_mempool_init(char * name, int elemt_size, int nb_elemts)
{
    int fd = 0;


	size_t mempool_size;
    uintptr_t map_addr = 0;

	shm_mempool_t *mempool = NULL;

	fd = open(name, O_RDWR |O_CREAT, 0666);

	if(fd == -1){
		return NULL;
	}

	mempool_size = nb_elemts * (sizeof(shm_elemt_t) + elemt_size) + sizeof(shm_mempool_t);

	if(ftruncate(fd, mempool_size) == -1){
		close(fd);
		return NULL;
	}

    map_addr =  align_address (SHM_POOL_BASE_ADDR + mempool_size);

	mempool = mmap((void *)map_addr, mempool_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd , 0);

	if(mempool == MAP_FAILED){
		close(fd);
		return NULL;
	}

	/* init the mempool structure */
	memset(mempool, 0, sizeof(shm_mempool_t));
    sprintf (mempool->name, "%s" , name);
	mempool->nb_elemts = nb_elemts;
    mempool->elemt_size = elemt_size;
	mempool->total_used = 0;

	mempool->mask = nb_elemts - 1;
    mempool->size = mempool_size;
	mempool->pos = 0;

    close(fd);

    //printf ("Create ===> nb_elemts: %d, mempool_size: %ld, %u\n", nb_elemts, mempool_size - sizeof(shm_mempool_t), mempool->size);
    return mempool;
}

/**
*功能: 查找内存池
*参数: name, mmap 映射的文件名
*返回值: 成功:内存池头指针 失败:NULL
*/
shm_mempool_t* shm_mempool_lookup(char *name)
{
    int fd = 0;
   uintptr_t map_addr = 0;

    shm_mempool_t * mempool = NULL;
    shm_mempool_t * mempool_tmp = NULL;

    fd = open(name, O_RDWR, 0666);
    if (fd == -1) {
		return NULL;
	}

    mempool_tmp = (shm_mempool_t*)mmap(NULL, sizeof(shm_mempool_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd , 0);
    if(mempool_tmp == MAP_FAILED ){
		close(fd);
		return NULL;
	}

    map_addr =  align_address (SHM_POOL_BASE_ADDR + mempool_tmp->size);
    mempool = (shm_mempool_t*)mmap((void *)map_addr, mempool_tmp->size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd , 0);
    if(mempool == MAP_FAILED  ){
		close(fd);
		return NULL;
	}

    close(fd);

    //printf ("LookUp ===> nb_elemts: %d, mempool_size: %ld\n", mempool->nb_elemts, mempool_size - sizeof(shm_mempool_t));
    return mempool;
}

/**
*功能: 从内存池中申请一个未使用的元素
*参数: 内存池指针
*返回值: 元素指针
*/
shm_elemt_t * shm_mempool_alloc(shm_mempool_t *mempool)
{
	int32_t success;
	uint32_t pos_cur, pos_next;
    uint32_t used_cur, used_next;

    uint32_t pos = 0;
    int32_t remain = 0;

    shm_elemt_t * elemt = NULL;

    if (! mempool) {
        printf ("shm_mempool is NULL failed: %d\n", __LINE__);
        return NULL;
    } 

	do { //提前占一个元素槽位
		used_cur = mempool->total_used;
		used_next = used_cur + 1;
        if (used_next >= mempool->nb_elemts) {
            //printf ("%s:%d shm_mempool remain [\033[1;31m failed \033[0m]: [nb_elemts: %d, total_used: %d]\n", __FILE__, __LINE__, mempool->nb_elemts, mempool->total_used);
            return NULL;
        }
		success = shm_mempool_atomic32_cmpset(&mempool->total_used, used_cur, used_next);
	} while (unlikely(success == 0));

	do {
		pos_cur = mempool->pos;
		pos_next = pos_cur + 1;
		success = shm_mempool_atomic32_cmpset(&mempool->pos, pos_cur, pos_next);
	} while (unlikely(success == 0));

	pos = pos_cur & mempool->mask; 
    //printf ("pos=%-4d, used=%-4d ", pos, mempool->total_used);

    //TODO: 取出指针后,还需要判断该指针的位是否正在使用,如果正在使用，还要向后取出pos,
    //再进行判断

    //TODO: 取出指针后,需要将元素位标记置为使用状态
    elemt = (shm_elemt_t*)((char *)mempool->data + pos * (sizeof(shm_elemt_t) + mempool->elemt_size));

    //申请出的内存目前还未释放掉
    if (elemt->used == 1) {
        //printf ("%s:%d [\033[1;31m failed \033[0m] [pid: %d]shm_mempool alloc [used]\n", __FILE__, __LINE__, getpid());
        return NULL;
    }

    elemt->used = 1;
    elemt->mempool = mempool;

    return elemt;
}

/**
*功能: 释放元素空间
*参数: 元素指针
*返回值: 无
*/
void shm_mempool_free(shm_elemt_t * elemt)
{
	int success;
    uint32_t used_cur, used_next;
    shm_mempool_t * mempool = NULL;

    if (!elemt) {
        return ;
    }
    
    mempool = ((shm_elemt_t *)elemt)->mempool;

	do {
		used_cur = mempool->total_used;
		used_next = used_cur - 1;
		success = shm_mempool_atomic32_cmpset(&mempool->total_used, used_cur, used_next);
	} while (unlikely(success == 0));

    elemt->used = 0;

    //TODO: 归还元素指针后,还需要将元素的记置为未使用

    return;
}

/**
*功能: 创建内存池
*参数: name, mmap 映射的文件名
*参数: elemt_size, 内存池中每个元素的大小
*参数: nb_elemts, 内存池中元素的个数
*返回值: 成功:内存池头指针 失败:NULL
*/
shm_mempool_t* shm_mempool_create(char * name, int elemt_size, int nb_elemts){
    if (access(name, F_OK)) {
        return shm_mempool_init(name, elemt_size, nb_elemts);
    }
    else {
        return shm_mempool_lookup(name);
        //return shm_mempool_lookup_1(name, elemt_size, nb_elemts);
    }

    return NULL;
}

/**
*功能: 销毁内存池
*参数: 内存池指针
*返回值: 无
*/
void shm_mempool_destroy(shm_mempool_t *mempool){
    int ret = 0;

    if (!mempool) {
        return ;
    }

    ret = munmap(mempool, mempool->size);
    
    if (!ret) {
        unlink(mempool->name);
    }
}
