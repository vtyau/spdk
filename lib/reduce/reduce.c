/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include "spdk/reduce.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk_internal/log.h"

#include "libpmem.h"

/* Always round up the size of the PM region to the nearest cacheline. */
#define REDUCE_PM_SIZE_ALIGNMENT	64

#define SPDK_REDUCE_SIGNATURE "SPDKREDU"

/* Offset into the backing device where the persistent memory file's path is stored. */
#define REDUCE_BACKING_DEV_PATH_OFFSET	4096

/* Structure written to offset 0 of both the pm file and the backing device. */
struct spdk_reduce_vol_superblock {
	uint8_t				signature[8];
	struct spdk_reduce_vol_params	params;
	uint8_t				reserved[4056];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_reduce_vol_superblock) == 4096, "size incorrect");

struct spdk_reduce_vol {
	struct spdk_uuid			uuid;
	struct spdk_reduce_pm_file		pm_file;
	struct spdk_reduce_backing_dev		*backing_dev;
	struct spdk_reduce_vol_superblock	*backing_super;
};

/*
 * Allocate extra metadata chunks and corresponding backing io units to account for
 *  outstanding IO in worst case scenario where logical map is completely allocated
 *  and no data can be compressed.  We need extra chunks in this case to handle
 *  in-flight writes since reduce never writes data in place.
 */
#define REDUCE_NUM_EXTRA_CHUNKS 128

static inline uint64_t
divide_round_up(uint64_t num, uint64_t divisor)
{
	return (num + divisor - 1) / divisor;
}

static uint64_t
_get_pm_logical_map_size(uint64_t vol_size, uint64_t chunk_size)
{
	uint64_t chunks_in_logical_map, logical_map_size;

	chunks_in_logical_map = vol_size / chunk_size;
	logical_map_size = chunks_in_logical_map * sizeof(uint64_t);

	/* Round up to next cacheline. */
	return divide_round_up(logical_map_size, REDUCE_PM_SIZE_ALIGNMENT) * REDUCE_PM_SIZE_ALIGNMENT;
}

static uint64_t
_get_total_chunks(uint64_t vol_size, uint64_t chunk_size)
{
	uint64_t num_chunks;

	num_chunks = vol_size / chunk_size;
	num_chunks += REDUCE_NUM_EXTRA_CHUNKS;

	return num_chunks;
}

static uint64_t
_get_pm_total_chunks_size(uint64_t vol_size, uint64_t chunk_size, uint64_t backing_io_unit_size)
{
	uint64_t io_units_per_chunk, num_chunks, total_chunks_size;

	num_chunks = _get_total_chunks(vol_size, chunk_size);
	io_units_per_chunk = chunk_size / backing_io_unit_size;
	total_chunks_size = num_chunks * io_units_per_chunk * sizeof(uint64_t);

	return divide_round_up(total_chunks_size, REDUCE_PM_SIZE_ALIGNMENT) * REDUCE_PM_SIZE_ALIGNMENT;
}

static int
_validate_vol_params(struct spdk_reduce_vol_params *params)
{
	if (params->vol_size == 0 || params->chunk_size == 0 || params->backing_io_unit_size == 0) {
		return -EINVAL;
	}

	/* Chunk size must be an even multiple of the backing io unit size. */
	if ((params->chunk_size % params->backing_io_unit_size) != 0) {
		return -EINVAL;
	}

	/* Volume size must be an even multiple of the chunk size. */
	if ((params->vol_size % params->chunk_size) != 0) {
		return -EINVAL;
	}

	return 0;
}

int64_t
spdk_reduce_get_pm_file_size(struct spdk_reduce_vol_params *params)
{
	uint64_t total_pm_size;
	int rc;

	rc = _validate_vol_params(params);
	if (rc != 0) {
		return rc;
	}

	total_pm_size = sizeof(struct spdk_reduce_vol_superblock);
	total_pm_size += _get_pm_logical_map_size(params->vol_size, params->chunk_size);
	total_pm_size += _get_pm_total_chunks_size(params->vol_size, params->chunk_size,
			 params->backing_io_unit_size);
	return total_pm_size;
}

int64_t
spdk_reduce_get_backing_device_size(struct spdk_reduce_vol_params *params)
{
	uint64_t total_backing_size, num_chunks;
	int rc;

	rc = _validate_vol_params(params);
	if (rc != 0) {
		return rc;
	}

	num_chunks = _get_total_chunks(params->vol_size, params->chunk_size);
	total_backing_size = num_chunks * params->chunk_size;
	total_backing_size += sizeof(struct spdk_reduce_vol_superblock);

	return total_backing_size;
}

struct reduce_init_load_ctx {
	struct spdk_reduce_vol			*vol;
	struct spdk_reduce_vol_cb_args		backing_cb_args;
	spdk_reduce_vol_op_with_handle_complete	cb_fn;
	void					*cb_arg;
	struct iovec				iov;
	void					*path;
};

static void
_init_write_super_cpl(void *cb_arg, int ziperrno)
{
	struct reduce_init_load_ctx *init_ctx = cb_arg;

	init_ctx->cb_fn(init_ctx->cb_arg, init_ctx->vol, ziperrno);
	free(init_ctx);
}

static void
_init_write_path_cpl(void *cb_arg, int ziperrno)
{
	struct reduce_init_load_ctx *init_ctx = cb_arg;
	struct spdk_reduce_vol *vol = init_ctx->vol;

	spdk_dma_free(init_ctx->path);
	init_ctx->iov.iov_base = vol->backing_super;
	init_ctx->iov.iov_len = sizeof(*vol->backing_super);
	init_ctx->backing_cb_args.cb_fn = _init_write_super_cpl;
	init_ctx->backing_cb_args.cb_arg = init_ctx;
	vol->backing_dev->writev(vol->backing_dev, &init_ctx->iov, 1,
				 0, sizeof(*vol->backing_super) / vol->backing_dev->blocklen,
				 &init_ctx->backing_cb_args);
}

void
spdk_reduce_vol_init(struct spdk_reduce_vol_params *params,
		     struct spdk_reduce_backing_dev *backing_dev,
		     struct spdk_reduce_pm_file *pm_file,
		     spdk_reduce_vol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_reduce_vol *vol;
	struct spdk_reduce_vol_superblock *pm_super;
	struct reduce_init_load_ctx *init_ctx;
	int64_t size, size_needed;
	int rc;

	rc = _validate_vol_params(params);
	if (rc != 0) {
		SPDK_ERRLOG("invalid vol params\n");
		cb_fn(cb_arg, NULL, rc);
		return;
	}

	size_needed = spdk_reduce_get_backing_device_size(params);
	size = backing_dev->blockcnt * backing_dev->blocklen;
	if (size_needed > size) {
		SPDK_ERRLOG("backing device size %" PRIi64 " but %" PRIi64 " needed\n",
			    size, size_needed);
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	size_needed = spdk_reduce_get_pm_file_size(params);
	size = pm_file->size;
	if (size_needed > size) {
		SPDK_ERRLOG("pm file size %" PRIi64 " but %" PRIi64 " needed\n",
			    size, size_needed);
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	if (spdk_mem_all_zero(&params->uuid, sizeof(params->uuid))) {
		SPDK_ERRLOG("no uuid specified\n");
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	if (backing_dev->close == NULL || backing_dev->readv == NULL ||
	    backing_dev->writev == NULL || backing_dev->unmap == NULL) {
		SPDK_ERRLOG("backing_dev function pointer not specified\n");
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	vol = calloc(1, sizeof(*vol));
	if (vol == NULL) {
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	memcpy(&vol->pm_file, pm_file, sizeof(*pm_file));

	vol->backing_super = spdk_dma_zmalloc(sizeof(*vol->backing_super), 0, NULL);
	if (vol->backing_super == NULL) {
		free(vol);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	init_ctx = calloc(1, sizeof(*init_ctx));
	if (init_ctx == NULL) {
		spdk_dma_free(vol->backing_super);
		free(vol);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	init_ctx->path = spdk_dma_zmalloc(REDUCE_PATH_MAX, 0, NULL);
	if (init_ctx->path == NULL) {
		free(init_ctx);
		spdk_dma_free(vol->backing_super);
		free(vol);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	memcpy(&vol->uuid, &params->uuid, sizeof(params->uuid));
	vol->backing_dev = backing_dev;

	memcpy(vol->backing_super->signature, SPDK_REDUCE_SIGNATURE,
	       sizeof(vol->backing_super->signature));
	memcpy(&vol->backing_super->params, params, sizeof(*params));

	pm_super = (struct spdk_reduce_vol_superblock *)vol->pm_file.pm_buf;
	memcpy(pm_super, vol->backing_super, sizeof(*vol->backing_super));

	if (vol->pm_file.pm_is_pmem) {
		pmem_persist(pm_super, sizeof(*pm_super));
	} else {
		pmem_msync(pm_super, sizeof(*pm_super));
	}

	init_ctx->vol = vol;
	init_ctx->cb_fn = cb_fn;
	init_ctx->cb_arg = cb_arg;

	memcpy(init_ctx->path, vol->pm_file.path, REDUCE_PATH_MAX);
	init_ctx->iov.iov_base = init_ctx->path;
	init_ctx->iov.iov_len = REDUCE_PATH_MAX;
	init_ctx->backing_cb_args.cb_fn = _init_write_path_cpl;
	init_ctx->backing_cb_args.cb_arg = init_ctx;
	/* Write path to offset 4K on backing device - just after where the super
	 *  block will be written.  We wait until this is committed before writing the
	 *  super block to guarantee we don't get the super block written without the
	 *  the path if the system crashed in the middle of a write operation.
	 */
	vol->backing_dev->writev(vol->backing_dev, &init_ctx->iov, 1,
				 REDUCE_BACKING_DEV_PATH_OFFSET / vol->backing_dev->blocklen,
				 REDUCE_PATH_MAX / vol->backing_dev->blocklen,
				 &init_ctx->backing_cb_args);
}

void
spdk_reduce_vol_unload(struct spdk_reduce_vol *vol,
		       spdk_reduce_vol_op_complete cb_fn, void *cb_arg)
{
	if (vol == NULL) {
		/* This indicates a programming error. */
		assert(false);
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	if (vol->pm_file.close != NULL) {
		vol->pm_file.close(&vol->pm_file);
	}

	vol->backing_dev->close(vol->backing_dev);

	spdk_dma_free(vol->backing_super);
	free(vol);
	cb_fn(cb_arg, 0);
}

SPDK_LOG_REGISTER_COMPONENT("reduce", SPDK_LOG_REDUCE)
