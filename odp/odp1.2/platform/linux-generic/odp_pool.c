/* Copyright (c) 2013, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

#include <odp/std_types.h>
#include <odp/pool.h>
#include <odp_buffer_internal.h>
#include <odp_pool_internal.h>
#include <odp_buffer_inlines.h>
#include <odp_packet_internal.h>
#include <odp_timer_internal.h>
#include <odp_align_internal.h>
#include <odp/shared_memory.h>
#include <odp/align.h>
#include <odp_internal.h>
#include <odp/config.h>
#include <odp/hints.h>
#include <odp_debug_internal.h>
#include <odp_atomic_internal.h>

#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include "odp_mmdistrict.h"

#if ODP_CONFIG_POOLS > ODP_BUFFER_MAX_POOLS
#error ODP_CONFIG_POOLS > ODP_BUFFER_MAX_POOLS
#endif

typedef union buffer_type_any_u {
	odp_buffer_hdr_t  buf;
	odp_packet_hdr_t  pkt;
	odp_timeout_hdr_t tmo;
} odp_anybuf_t;

/* Any buffer type header */
typedef struct {
	union buffer_type_any_u any_hdr; /* any buffer type */
} odp_any_buffer_hdr_t;

typedef struct odp_any_hdr_stride {
	uint8_t pad[ODP_CACHE_LINE_SIZE_ROUNDUP(sizeof(odp_any_buffer_hdr_t))];
} odp_any_hdr_stride;

typedef struct pool_table_t {
	pool_entry_t pool[ODP_CONFIG_POOLS];
} pool_table_t;

/* The pool table */
static pool_table_t *pool_tbl;
static const char SHM_DEFAULT_NAME[] = "odp_buffer_pools";

/* Pool entry pointers (for inlining) */
void *pool_entry_ptr[ODP_CONFIG_POOLS];

unsigned char pool_type_name[][32] = {
	"NULL", "buffer", "packet", "timeout"
};

/* Local cache for buffer alloc/free acceleration */
static __thread local_cache_t local_cache[ODP_CONFIG_POOLS];

int odp_pool_init_global(void)
{
	uint32_t i;
	odp_shm_t shm;
	pool_entry_t *pool;

	shm = odp_shm_reserve(SHM_DEFAULT_NAME,
			      sizeof(pool_table_t),
			      sizeof(pool_entry_t), 0);

	pool_tbl = odp_shm_addr(shm);

	if (pool_tbl == NULL)
		return -1;

	memset(pool_tbl, 0, sizeof(pool_table_t));

	for (i = 0; i < ODP_CONFIG_POOLS; i++) {
		/* init locks */
		pool = &pool_tbl->pool[i];
		POOL_LOCK_INIT(&pool->s.lock);
		POOL_LOCK_INIT(&pool->s.buf_lock);
		POOL_LOCK_INIT(&pool->s.blk_lock);
		pool->s.pool_hdl  = pool_index_to_handle(i);
		pool->s.pool_id	  = i;
		pool_entry_ptr[i] = pool;
		odp_atomic_init_u32(&pool->s.bufcount, 0);
		odp_atomic_init_u32(&pool->s.blkcount, 0);

		/* Initialize pool statistics counters */
		odp_atomic_init_u64(&pool->s.poolstats.bufallocs, 0);
		odp_atomic_init_u64(&pool->s.poolstats.buffrees, 0);
		odp_atomic_init_u64(&pool->s.poolstats.blkallocs, 0);
		odp_atomic_init_u64(&pool->s.poolstats.blkfrees, 0);
		odp_atomic_init_u64(&pool->s.poolstats.bufempty, 0);
		odp_atomic_init_u64(&pool->s.poolstats.blkempty, 0);
		odp_atomic_init_u64(&pool->s.poolstats.high_wm_count, 0);
		odp_atomic_init_u64(&pool->s.poolstats.low_wm_count, 0);
	}

	ODP_DBG("  Pool init global\n");
	ODP_DBG("  pool_entry_s size     %zu\n", sizeof(struct pool_entry_s));
	ODP_DBG("  pool_entry_t size     %zu\n", sizeof(pool_entry_t));
	ODP_DBG("  odp_buffer_hdr_t size %zu\n", sizeof(odp_buffer_hdr_t));
	ODP_DBG("\n");
	return 0;
}

int odp_pool_term_global(void)
{
	int i;
	pool_entry_t *pool;
	int ret = 0;
	int rc	= 0;

	for (i = 0; i < ODP_CONFIG_POOLS; i++) {
		pool = get_pool_entry(i);

		POOL_LOCK(&pool->s.lock);
		if (pool->s.pool_shm != ODP_SHM_INVALID) {
			ODP_ERR("Not destroyed pool: %s\n", pool->s.name);
			rc = -1;
		}

		POOL_UNLOCK(&pool->s.lock);
	}

	ret = odp_shm_free(odp_shm_lookup(SHM_DEFAULT_NAME));
	if (ret < 0) {
		ODP_ERR("shm free failed for %s", SHM_DEFAULT_NAME);
		rc = -1;
	}

	return rc;
}

int odp_pool_term_local(void)
{
	_odp_flush_caches();
	return 0;
}

/**
 * Pool creation
 */
odp_pool_t odp_pool_create(const char *name, odp_pool_param_t *params)
{
	odp_pool_t pool_hdl = ODP_POOL_INVALID;
	pool_entry_t *pool;
	uint32_t   i, headroom = 0, tailroom = 0;
	unsigned int pool_id;

	if (params == NULL)
		return ODP_POOL_INVALID;

	/* Default size and align for timeouts */
	if (params->type == ODP_POOL_TIMEOUT) {
		params->buf.size  = 0;
		params->buf.align = 0;
	}

	if (params->type == ODP_POOL_PACKET)
		return odp_pool_create_packet(name, params);

	/* Default initialization parameters */
	uint32_t p_udata_size = 0;
	uint32_t udata_stride = 0;

	/* Restriction for v1.0: All non-packet buffers are unsegmented */
	int unseg = 1;

	/* Restriction for v1.0: No zeroization support */
	const int zeroized = 0;

	uint32_t  blk_size, buf_stride, buf_num, seg_len = 0;
	uint32_t  buf_align = params->type == ODP_POOL_BUFFER ? params->buf.align : 0;

	/* Validate requested buffer alignment */
	if ((buf_align > ODP_CONFIG_BUFFER_ALIGN_MAX)
	    || (buf_align != ODP_ALIGN_ROUNDDOWN_POWER_2(buf_align, buf_align)))
		return ODP_POOL_INVALID;

	/* Set correct alignment based on input request */
	if (buf_align == 0)
		buf_align = ODP_CACHE_LINE_SIZE;
	else if (buf_align < ODP_CONFIG_BUFFER_ALIGN_MIN)
		buf_align = ODP_CONFIG_BUFFER_ALIGN_MIN;

	/* Calculate space needed for buffer blocks and metadata */

	switch (params->type) {
	case ODP_POOL_BUFFER:
		buf_num	 = params->buf.num;
		blk_size = params->buf.size;

		/* Optimize small raw buffers */
		if ((blk_size > ODP_MAX_INLINE_BUF) || (params->buf.align != 0))
			blk_size = ODP_ALIGN_ROUNDUP(blk_size, buf_align);

		buf_stride = sizeof(odp_buffer_hdr_stride);
		break;
	case ODP_POOL_TIMEOUT:
		blk_size = 0;
		buf_num	 = params->tmo.num;
		buf_stride = sizeof(odp_timeout_hdr_stride);
		break;

	default:
		return ODP_POOL_INVALID;
	}

	/* Validate requested number of buffers against addressable limits */
	if (buf_num >
	    (ODP_BUFFER_MAX_BUFFERS / (buf_stride / ODP_CACHE_LINE_SIZE)))
		return ODP_POOL_INVALID;

	/* Find an unused buffer pool slot and iniitalize it as requested */

	for (i = 0; i < ODP_CONFIG_POOLS; i++) {
		pool = get_pool_entry(i);

		POOL_LOCK(&pool->s.lock);
		if (pool->s.pool_shm != ODP_SHM_INVALID) {
			POOL_UNLOCK(&pool->s.lock);
			continue;
		} else {
			pool_hdl = pool->s.pool_hdl;
			break;
		}
	}

	if (pool_hdl == ODP_POOL_INVALID)
		return ODP_POOL_INVALID;

	/* found free pool */
	size_t block_size, pad_size, mdata_size, udata_size;

	pool->s.flags.all = 0;

	if (name == NULL) {
		pool->s.name[0] = 0;
	} else {
		strncpy(pool->s.name, name,
			ODP_POOL_NAME_LEN - 1);
		pool->s.name[ODP_POOL_NAME_LEN - 1] = 0;
		pool->s.flags.has_name = 1;
	}

	pool->s.params = *params;
	pool->s.buf_align = buf_align;

	/* Optimize for short buffers: Data stored in buffer hdr */
	if (blk_size <= ODP_MAX_INLINE_BUF) {
		block_size = 0;
		pool->s.buf_align = blk_size == 0 ? 0 : sizeof(void *);
	} else {
		block_size = buf_num * blk_size;
		pool->s.buf_align = buf_align;
	}

	pad_size = ODP_CACHE_LINE_SIZE_ROUNDUP(block_size) - block_size;
	mdata_size = buf_num * buf_stride;
	udata_size = buf_num * udata_stride;

	pool->s.buf_num = buf_num;
	pool->s.pool_size = ODP_PAGE_SIZE_ROUNDUP(block_size +
						  pad_size +
						  mdata_size +
						  udata_size);

	const struct odp_mm_district *zone = NULL;
	char memdistrict_name[ODP_POOL_NAME_LEN + 8];
	int pid = getpid();
	snprintf(memdistrict_name, sizeof(memdistrict_name), "%s%d", pool->s.name, pid);
	zone = odp_mm_district_reserve(memdistrict_name, pool->s.pool_size, 0, ODP_MEMZONE_2MB | ODP_MEMZONE_SIZE_HINT_ONLY);
	if (zone == NULL) {
		POOL_UNLOCK(&pool->s.lock);
		return ODP_POOL_INVALID;
	}

	pool->s.pool_base_addr = (unsigned long)zone->addr;
	pool->s.pool_shm = (odp_shm_t)1;

	/* Now safe to unlock since pool entry has been allocated */
	POOL_UNLOCK(&pool->s.lock);

	pool->s.flags.unsegmented = unseg;
	pool->s.flags.zeroized = zeroized;
	pool->s.seg_size = unseg ? blk_size : seg_len;
	pool->s.blk_size = blk_size;

	uint8_t *block_base_addr = zone->addr;
	uint8_t *mdata_base_addr = block_base_addr + block_size + pad_size;
	uint8_t *udata_base_addr = mdata_base_addr + mdata_size;

	/* Pool mdata addr is used for indexing buffer metadata */
	pool->s.pool_mdata_addr = mdata_base_addr;
	pool->s.udata_size = p_udata_size;

	pool->s.buf_stride = buf_stride;
	pool->s.buf_freelist = NULL;
	pool->s.blk_freelist = NULL;

	/* Initialization will increment these to their target vals */
	odp_atomic_store_u32(&pool->s.bufcount, 0);
	odp_atomic_store_u32(&pool->s.blkcount, 0);

	uint8_t *buf  = udata_base_addr - buf_stride;
	uint8_t *udat = udata_stride == 0 ? NULL :
			udata_base_addr + udata_size - udata_stride;

	/* Init buffer common header and add to pool buffer freelist */
	do {
		odp_buffer_hdr_t *tmp = (odp_buffer_hdr_t *)(void *)buf;

		/* Iniitalize buffer metadata */
		tmp->allocator = ODP_FREEBUF;
		tmp->flags.all = 0;
		tmp->flags.zeroized = zeroized;
		tmp->size = 0;
		odp_atomic_init_u32(&tmp->ref_count, 0);
		tmp->type = params->type;
		tmp->event_type = params->type;
		tmp->pool_hdl = pool->s.pool_hdl;
		tmp->uarea_addr = (void *)udat;
		tmp->uarea_size = p_udata_size;
		tmp->segcount = 0;
		tmp->segsize  = pool->s.seg_size;
		tmp->handle.handle = odp_buffer_encode_handle(tmp);

		/* Set 1st seg addr for zero-len buffers */
		tmp->addr[0] = NULL;

		/* Special case for short buffer data */
		if (blk_size <= ODP_MAX_INLINE_BUF) {
			tmp->flags.hdrdata = 1;
			if (blk_size > 0) {
				tmp->segcount = 1;
				tmp->addr[0]  = &tmp->addr[1];
				tmp->size = blk_size;
			}
		}

		/* Push buffer onto pool's freelist */
		ret_buf(&pool->s, tmp);
		buf  -= buf_stride;
		udat -= udata_stride;
	} while (buf >= mdata_base_addr);

	/* Form block freelist for pool */
	uint8_t *blk = block_base_addr + block_size - pool->s.seg_size;

	if (blk_size > ODP_MAX_INLINE_BUF)
		do {
			ret_blk(&pool->s, blk);
			blk -= pool->s.seg_size;
		} while (blk >= block_base_addr);

	/* Initialize pool statistics counters */
	odp_atomic_store_u64(&pool->s.poolstats.bufallocs, 0);
	odp_atomic_store_u64(&pool->s.poolstats.buffrees, 0);
	odp_atomic_store_u64(&pool->s.poolstats.blkallocs, 0);
	odp_atomic_store_u64(&pool->s.poolstats.blkfrees, 0);
	odp_atomic_store_u64(&pool->s.poolstats.bufempty, 0);
	odp_atomic_store_u64(&pool->s.poolstats.blkempty, 0);
	odp_atomic_store_u64(&pool->s.poolstats.high_wm_count, 0);
	odp_atomic_store_u64(&pool->s.poolstats.low_wm_count, 0);

	/* Reset other pool globals to initial state */
	pool->s.low_wm_assert = 0;
	pool->s.quiesced = 0;
	pool->s.headroom = headroom;
	pool->s.tailroom = tailroom;

	/* Watermarks are hard-coded for now to control caching */
	pool->s.high_wm = buf_num / 2;
	pool->s.low_wm	= buf_num / 4;

	pool_hdl = pool->s.pool_hdl;

	pool_id = pool_handle_to_index(pool_hdl);

	PRINT("pool_id: %u, pool_type: %s, pool_name: %s\n", pool_id, pool_type_name[params->type], pool->s.name);
	PRINT("num: %u, buf_len: %u, pad_size: %lld, size: 0x%lx, pool_size: 0x%lx.\n",
	      buf_num, (unsigned int)blk_size, (unsigned long long)pad_size, block_size, pool->s.pool_size);
	PRINT("addr: 0x%lx, phys_addr: 0x%lx\n", zone->addr_64, zone->phys_addr);

	PRINT("\n");

	return pool_hdl;
}

odp_pool_t odp_pool_create_packet(const char *name, odp_pool_param_t *params)
{
	int i = 0;
	const int zeroized = 0;
	unsigned int buf_num = 0;
	unsigned int buf_len = 0;
	unsigned int seg_len = 0;
	unsigned int buf_stride = 0;
	unsigned int buf_head = 0;
	unsigned int pool_id;
	unsigned long long stride_size;
	unsigned long long block_size;
	unsigned long long head_size;
	unsigned long long pool_size;
	unsigned long long pad_size;
	odp_pool_t pool_hdl = ODP_POOL_INVALID;
	pool_entry_t *pool;

	if (params == NULL) {
		PRINT("params is null.\n");
		return ODP_POOL_INVALID;
	}

	buf_num = params->pkt.num;
	buf_len = params->pkt.len;
	seg_len = params->pkt.seg_len;

	if (buf_len < (PACKET_MIN_SIZE + PACKET_HEAD_SIZE + seg_len))
		buf_len = PACKET_MIN_SIZE + PACKET_HEAD_SIZE + seg_len;

	buf_len	 = ODP_ALIGN_ROUNDUP(buf_len, ODP_CACHE_LINE_SIZE);
	buf_head = PACKET_HEAD_SIZE * 2;
	if (seg_len > 0)
		buf_stride = sizeof(odp_packet_hdr_stride);
	else
		buf_stride = 0;

	/* Find an unused buffer pool slot and iniitalize it as requested */
	for (i = 0; i < ODP_CONFIG_POOLS; i++) {
		pool = get_pool_entry(i);

		POOL_LOCK(&pool->s.lock);
		if (pool->s.pool_shm != ODP_SHM_INVALID) {
			POOL_UNLOCK(&pool->s.lock);
			continue;
		} else {
			pool_hdl = pool->s.pool_hdl;
			break;
		}
	}

	if (pool_hdl == ODP_POOL_INVALID)
		return ODP_POOL_INVALID;

	pool_id = pool_handle_to_index(pool_hdl);
	pool->s.flags.all = 0;

	if (name == NULL) {
		pool->s.name[0] = 0;
	} else {
		strncpy(pool->s.name, name, ODP_POOL_NAME_LEN - 1);
		pool->s.name[ODP_POOL_NAME_LEN - 1] = 0;
		pool->s.flags.has_name = 1;
	}

	head_size = buf_num * buf_head;
	stride_size = buf_num * buf_stride;
	block_size  = buf_num * buf_len;
	pool_size = block_size + head_size + stride_size;
	pad_size  = ODP_ALIGN_ROUNDUP(pool_size, POOL_BUF_ALIGN_SIZE) - pool_size;
	pool->s.pool_size = pool_size + pad_size;

	pool->s.params	= *params;
	pool->s.buf_num = buf_num * 2;

	const struct odp_mm_district *zone = NULL;

	zone = odp_mm_district_reserve(pool->s.name, pool->s.pool_size, 0,
				   ODP_MEMZONE_2MB | ODP_MEMZONE_SIZE_HINT_ONLY);
	if (zone == NULL) {
		POOL_UNLOCK(&pool->s.lock);
		return ODP_POOL_INVALID;
	}

	pool->s.pool_base_addr = (unsigned long)zone->addr;
	pool->s.pool_base_phy_addr = zone->phys_addr;
	pool->s.pool_shm = (odp_shm_t)1;

	/* Now safe to unlock since pool entry has been allocated */
	POOL_UNLOCK(&pool->s.lock);

	uint8_t *block_base_addr  = zone->addr;
	uint8_t *mdata_base_addr  = block_base_addr + block_size;
	uint8_t *packet_base_addr = mdata_base_addr + head_size;

	pool->s.flags.unsegmented = 1;
	pool->s.flags.zeroized = zeroized;
	pool->s.blk_size = buf_len;
	pool->s.seg_size = seg_len;

	/* Pool mdata addr is used for indexing buffer metadata */
	pool->s.pool_mdata_addr	 = mdata_base_addr;
	pool->s.pool_malloc_addr = mdata_base_addr;
	pool->s.pool_free_addr = mdata_base_addr + buf_num * PACKET_HEAD_SIZE;
	pool->s.pool_packet_addr = packet_base_addr;

	pool->s.malloc_index = 0;
	pool->s.free_index = buf_num;

	pool->s.buf_freelist = NULL;
	pool->s.blk_freelist = NULL;

	pool->s.udata_size = head_size;
	pool->s.buf_stride = stride_size;
	pool->s.low_wm_assert = 0;
	pool->s.quiesced = 0;
	pool->s.headroom = seg_len;
	pool->s.tailroom = 0;
	pool->s.high_wm	 = buf_num / 2;
	pool->s.low_wm = buf_num / 4;

	if (params->pkt.lock > 0) {
		pool->s.odp_pool_packet_alloc = (pool_packet_alloc)_packet_alloc_lock;
		pool->s.odp_pool_packet_free = (pool_packet_free)_packet_free_lock;
	}
	else {
		pool->s.odp_pool_packet_alloc = (pool_packet_alloc)_packet_alloc;
		pool->s.odp_pool_packet_free = (pool_packet_free)_packet_free;
	}

	/* Initialization will increment these to their target vals */
	odp_atomic_store_u32(&pool->s.bufcount, 0);
	odp_atomic_store_u32(&pool->s.blkcount, 0);

	memset(mdata_base_addr, 0, head_size + stride_size);

	packet_head_t *buf_addr;
	packet_head_t *buf_hear_malloc = NULL;
	packet_head_t *buf_hear_free = NULL;
	odp_packet_hdr_t *buf_packet_hdr;
	unsigned long  vir_addr = zone->addr_64 + PACKET_HEAD_SIZE + seg_len;
	unsigned long long phy_addr = zone->phys_addr + PACKET_HEAD_SIZE + seg_len;

	for (i = 0; i < buf_num; i++) {
		buf_addr = (packet_head_t *)(block_base_addr + i * buf_len);
		buf_hear_malloc = (packet_head_t *)(pool->s.pool_malloc_addr + i * PACKET_HEAD_SIZE);
		buf_hear_free = (packet_head_t *)(pool->s.pool_free_addr + i * PACKET_HEAD_SIZE);
		if (seg_len > 0) {
			buf_packet_hdr =
				(odp_packet_hdr_t *)(packet_base_addr + i * sizeof(odp_packet_hdr_stride));
			buf_packet_hdr->buf_addr = (unsigned char *)vir_addr;
		} else {
			buf_packet_hdr = 0;
		}

		buf_addr->vir_addr = vir_addr;
		buf_addr->phy_addr = phy_addr;
		buf_addr->pool_id  = pool_id;
		buf_addr->index = i;
		buf_addr->packet_hdr = (unsigned long)buf_packet_hdr;
		buf_addr->headroom = seg_len;
		buf_addr->tailroom = 0;
		SET_HEADROOM(vir_addr, seg_len);

		buf_hear_malloc->vir_addr = vir_addr;
		buf_hear_malloc->phy_addr = phy_addr;
		buf_hear_malloc->pool_id  = pool_id;
		buf_hear_malloc->index = i;
		buf_hear_malloc->packet_hdr = (unsigned long)buf_packet_hdr;
		buf_hear_malloc->headroom = seg_len;
		buf_hear_malloc->tailroom = 0;
		
		buf_hear_free->pool_id  = pool_id;
		buf_hear_free->headroom = seg_len;
		buf_hear_free->tailroom = 0;
		
		vir_addr += buf_len;
		phy_addr += buf_len;
	}

	PRINT("pool_id: %u, pool_type: %s, pool_name: %s\n", pool_id, pool_type_name[params->type], pool->s.name);
	PRINT("num: %u, buf_len: %u, seg_len: %u, pad_size: %lld, size: 0x%llx, pool_size: 0x%lx.\n",
	      buf_num, buf_len, seg_len, pad_size, block_size, pool->s.pool_size);
	PRINT("addr: 0x%lx, phys_addr: 0x%lx\n", zone->addr_64, zone->phys_addr);
	PRINT("\n");

	return pool_hdl;
}

odp_pool_t odp_pool_lookup(const char *name)
{
	uint32_t i;
	pool_entry_t *pool;

	for (i = 0; i < ODP_CONFIG_POOLS; i++) {
		pool = get_pool_entry(i);

		POOL_LOCK(&pool->s.lock);
		if (strcmp(name, pool->s.name) == 0) {
			/* found it */
			POOL_UNLOCK(&pool->s.lock);
			return pool->s.pool_hdl;
		}

		POOL_UNLOCK(&pool->s.lock);
	}

	return ODP_POOL_INVALID;
}

int odp_pool_info(odp_pool_t	   pool_hdl,
		  odp_pool_info_t *info)
{
	uint32_t pool_id = pool_handle_to_index(pool_hdl);
	pool_entry_t *pool = get_pool_entry(pool_id);

	if ((pool == NULL) || (info == NULL))
		return -1;

	info->name = pool->s.name;
	info->params = pool->s.params;

	return 0;
}

int odp_pool_destroy(odp_pool_t pool_hdl)
{
	uint32_t pool_id = pool_handle_to_index(pool_hdl);
	pool_entry_t *pool = get_pool_entry(pool_id);

	if (pool == NULL)
		return -1;

	POOL_LOCK(&pool->s.lock);

	/* Call fails if pool is not allocated or predefined*/
	if ((pool->s.pool_shm == ODP_SHM_INVALID) ||
	    pool->s.flags.predefined) {
		POOL_UNLOCK(&pool->s.lock);
		return -1;
	}

	/* Make sure local cache is empty */
	flush_cache(&local_cache[pool_id], &pool->s);

	/* Call fails if pool has allocated buffers */
	if (odp_atomic_load_u32(&pool->s.bufcount) < pool->s.buf_num) {
		POOL_UNLOCK(&pool->s.lock);
		return -1;
	}

	odp_shm_free(pool->s.pool_shm);
	pool->s.pool_shm = ODP_SHM_INVALID;
	POOL_UNLOCK(&pool->s.lock);

	return 0;
}

unsigned long odp_get_pool_vir_addr(
	odp_pool_t pool_hdl)
{
	pool_entry_t *pool = odp_pool_to_entry(pool_hdl);

	return pool->s.pool_base_addr;
}

unsigned long long odp_get_pool_phy_addr(
	odp_pool_t
	pool_hdl)
{
	pool_entry_t *pool = odp_pool_to_entry(pool_hdl);

	return pool->s.pool_base_phy_addr;
}

odp_buffer_t buffer_alloc(odp_pool_t
			  pool_hdl,
			  size_t size)
{
	uint32_t pool_id = pool_handle_to_index(pool_hdl);
	pool_entry_t *pool = get_pool_entry(pool_id);
	uintmax_t totsize  = pool->s.headroom + size + pool->s.tailroom;
	odp_anybuf_t *buf;

	/* Reject oversized allocation requests */
	if ((pool->s.flags.unsegmented && (totsize > pool->s.seg_size)) ||
	    (!pool->s.flags.unsegmented &&
	     (totsize > pool->s.seg_size * ODP_BUFFER_MAX_SEG)))
		return ODP_BUFFER_INVALID;

	/* Try to satisfy request from the local cache */
	buf = (odp_anybuf_t *)(void *)get_local_buf(&local_cache[pool_id],
						    &pool->s, totsize);

	/* If cache is empty, satisfy request from the pool */
	if (odp_unlikely(buf == NULL)) {
		buf = (odp_anybuf_t *)(void *)get_buf(&pool->s);

		if (odp_unlikely(buf == NULL))
			return ODP_BUFFER_INVALID;

		/* Get blocks for this buffer, if pool uses application data */
		if (buf->buf.size < totsize) {
			intmax_t needed = totsize - buf->buf.size;

			do {
				uint8_t *blk = get_blk(&pool->s);

				if (blk == NULL) {
					ret_buf(&pool->s, &buf->buf);
					return ODP_BUFFER_INVALID;
				}

				buf->buf.addr[buf->buf.segcount++] = blk;
				needed -= pool->s.seg_size;
			} while (needed > 0);

			buf->buf.size = buf->buf.segcount * pool->s.seg_size;
		}
	}

	/* By default, buffers inherit their pool's zeroization setting */
	buf->buf.flags.zeroized = pool->s.flags.zeroized;

	if (buf->buf.type == ODP_EVENT_PACKET)
		packet_init(pool, &buf->pkt, size);

	return odp_hdr_to_buf(&buf->buf);
}

odp_buffer_t odp_buffer_alloc(odp_pool_t pool_hdl)
{
	return buffer_alloc(pool_hdl,
			    odp_pool_to_entry(pool_hdl)->s.params.buf.size);
}

void odp_buffer_free(odp_buffer_t buf)
{
	odp_buffer_hdr_t *buf_hdr = odp_buf_to_hdr(buf);
	pool_entry_t	 *pool = odp_buf_to_pool(buf_hdr);

	if (odp_unlikely(pool->s.low_wm_assert))
		ret_buf(&pool->s, buf_hdr);
	else
		ret_local_buf(&local_cache[pool->s.pool_id], buf_hdr);
}

void _odp_flush_caches(void)
{
	int i;

	for (i = 0; i < ODP_CONFIG_POOLS; i++) {
		pool_entry_t *pool = get_pool_entry(i);

		flush_cache(&local_cache[i], &pool->s);
	}
}

void odp_pool_print(odp_pool_t pool_hdl)
{
	pool_entry_t *pool;
	uint32_t pool_id;

	pool_id = pool_handle_to_index(pool_hdl);
	pool = get_pool_entry(pool_id);

	uint32_t bufcount  = odp_atomic_load_u32(&pool->s.bufcount);
	uint32_t blkcount  = odp_atomic_load_u32(&pool->s.blkcount);
	uint64_t bufallocs = odp_atomic_load_u64(&pool->s.poolstats.bufallocs);
	uint64_t buffrees  = odp_atomic_load_u64(&pool->s.poolstats.buffrees);
	uint64_t blkallocs = odp_atomic_load_u64(&pool->s.poolstats.blkallocs);
	uint64_t blkfrees  = odp_atomic_load_u64(&pool->s.poolstats.blkfrees);
	uint64_t bufempty  = odp_atomic_load_u64(&pool->s.poolstats.bufempty);
	uint64_t blkempty  = odp_atomic_load_u64(&pool->s.poolstats.blkempty);
	uint64_t hiwmct =
		odp_atomic_load_u64(&pool->s.poolstats.high_wm_count);
	uint64_t lowmct =
		odp_atomic_load_u64(&pool->s.poolstats.low_wm_count);

	ODP_DBG("Pool info\n");
	ODP_DBG("---------\n");
	ODP_DBG(" pool            %" PRIu64 "\n",
		odp_pool_to_u64(pool->s.pool_hdl));
	ODP_DBG(" name            %s\n",
		pool->s.flags.has_name ? pool->s.name : "Unnamed Pool");
	ODP_DBG(" pool type       %s\n",
		pool->s.params.type == ODP_POOL_BUFFER ? "buffer" :
		(pool->s.params.type == ODP_POOL_PACKET ? "packet" :
		 (pool->s.params.type == ODP_POOL_TIMEOUT ? "timeout" :
		  "unknown")));
	ODP_DBG(" pool storage    ODP managed shm handle %" PRIu64 "\n",
		odp_shm_to_u64(pool->s.pool_shm));
	ODP_DBG(" pool status     %s\n",
		pool->s.quiesced ? "quiesced" : "active");
	ODP_DBG(" pool opts       %s, %s, %s\n",
		pool->s.flags.unsegmented ? "unsegmented" : "segmented",
		pool->s.flags.zeroized ? "zeroized" : "non-zeroized",
		pool->s.flags.predefined  ? "predefined" : "created");
	ODP_DBG(" pool base       %p\n", pool->s.pool_base_addr);
	ODP_DBG(" pool size       %zu (%zu pages)\n",
		pool->s.pool_size, pool->s.pool_size / ODP_PAGE_SIZE);
	ODP_DBG(" pool mdata base %p\n", pool->s.pool_mdata_addr);
	ODP_DBG(" udata size      %zu\n", pool->s.udata_size);
	ODP_DBG(" headroom        %u\n", pool->s.headroom);
	ODP_DBG(" tailroom        %u\n", pool->s.tailroom);
	if (pool->s.params.type == ODP_POOL_BUFFER) {
		ODP_DBG(" buf size        %zu\n", pool->s.params.buf.size);
		ODP_DBG(" buf align       %u requested, %u used\n",
			pool->s.params.buf.align, pool->s.buf_align);
	} else if (pool->s.params.type == ODP_POOL_PACKET) {
		ODP_DBG(" seg length      %u requested, %u used\n",
			pool->s.params.pkt.seg_len, pool->s.seg_size);
		ODP_DBG(" pkt length      %u requested, %u used\n",
			pool->s.params.pkt.len, pool->s.blk_size);
	}

	ODP_DBG(" num bufs        %u\n", pool->s.buf_num / 2);
	ODP_DBG(" bufs available  %u %s\n", bufcount,
		pool->s.low_wm_assert ? " **low wm asserted**" : "");
	ODP_DBG(" bufs in use     %u\n", pool->s.buf_num / 2 - bufcount);
	ODP_DBG(" buf allocs      %lu\n", bufallocs);
	ODP_DBG(" buf frees       %lu\n", buffrees);
	ODP_DBG(" buf empty       %lu\n", bufempty);
	ODP_DBG(" blk size        %zu\n", pool->s.seg_size);
	ODP_DBG(" blks available  %u\n", blkcount);
	ODP_DBG(" blk allocs      %lu\n", blkallocs);
	ODP_DBG(" blk frees       %lu\n", blkfrees);
	ODP_DBG(" blk empty       %lu\n", blkempty);
	ODP_DBG(" high wm value   %lu\n", pool->s.high_wm);
	ODP_DBG(" high wm count   %lu\n", hiwmct);
	ODP_DBG(" low wm value    %lu\n", pool->s.low_wm);
	ODP_DBG(" low wm count    %lu\n", lowmct);
}

odp_pool_t odp_buffer_pool(odp_buffer_t buf)
{
	return odp_buf_to_hdr(buf)->pool_hdl;
}
