#include "scrmfs-runtime-config.h"
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <search.h>
#include <assert.h>
#include <libgen.h>
#include <limits.h>
#define __USE_GNU
#include <pthread.h>

#include "scrmfs-internal.h"

static int    scrmfs_max_files;  /* maximum number of files to store */
static size_t scrmfs_chunk_mem;  /* number of bytes in memory to be used for chunk storage */
static int    scrmfs_chunk_bits; /* we set chunk size = 2^scrmfs_chunk_bits */
static off_t  scrmfs_chunk_size; /* chunk size in bytes */
static off_t  scrmfs_chunk_mask; /* mask applied to logical offset to determine physical offset within chunk */
static int    scrmfs_max_chunks; /* maximum number of chunks that fit in memory */

/* given a file id and logical chunk id, return pointer to meta data
 * for specified chunk, return NULL if not found */
static scrmfs_chunkmeta_t* scrmfs_get_chunkmeta(int fid, int cid)
{
    /* lookup file meta data for specified file id */
    scrmfs_filemeta_t* meta = scrmfs_get_meta_from_fid(fid);
    if (meta != NULL) {
        /* now lookup chunk meta data for specified chunk id */
        if (cid >= 0 && cid < scrmfs_max_chunks) {
           scrmfs_chunkmeta_t* chunk_meta = &(meta->chunk_meta[cid]);
           return chunk_meta;
        }
    }

    /* failed to find file or chunk id is out of range */
    return (scrmfs_chunkmeta_t *)NULL;
}

/* ---------------------------------------
 * Operations on file chunks
 * --------------------------------------- */

/* given a logical chunk id and an offset within that chunk, return the pointer
 * to the memory location corresponding to that location */
static inline void* scrmfs_compute_chunk_buf(
  const scrmfs_filemeta_t* meta,
  int logical_id,
  off_t logical_offset)
{
    /* get pointer to chunk meta */
    const scrmfs_chunkmeta_t* chunk_meta = &(meta->chunk_meta[logical_id]);

    /* identify physical chunk id */
    int physical_id = chunk_meta->id;

    /* compute the start of the chunk */
    char *start = NULL;
    if (physical_id < scrmfs_max_chunks) {
        start = scrmfs_chunks + (physical_id << scrmfs_chunk_bits);
    } else {
        /* chunk is in spill over */
        debug("wrong chunk ID\n");
        return NULL;
    }

    /* now add offset */
    char* buf = start + logical_offset;
    return (void*)buf;
}

/* given a chunk id and an offset within that chunk, return the offset
 * in the spillover file corresponding to that location */
static inline off_t scrmfs_compute_spill_offset(
  const scrmfs_filemeta_t* meta,
  int logical_id,
  off_t logical_offset)
{
    /* get pointer to chunk meta */
    const scrmfs_chunkmeta_t* chunk_meta = &(meta->chunk_meta[logical_id]);

    /* identify physical chunk id */
    int physical_id = chunk_meta->id;

    /* compute start of chunk in spill over device */
    off_t start = 0;
    if (physical_id < scrmfs_max_chunks) {
        debug("wrong spill-chunk ID\n");
        return -1;
    } else {
        /* compute buffer loc within spillover device chunk */
        /* account for the scrmfs_max_chunks added to identify location when
         * grabbing this chunk */
        start = ((physical_id - scrmfs_max_chunks) << scrmfs_chunk_bits);
    }
    off_t buf = start + logical_offset;
    return buf;
}

/* allocate a new chunk for the specified file and logical chunk id */
static int scrmfs_chunk_alloc(int fid, scrmfs_filemeta_t* meta, int chunk_id)
{
    /* get pointer to chunk meta data */
    scrmfs_chunkmeta_t* chunk_meta = &(meta->chunk_meta[chunk_id]);
    
    /* allocate a chunk and record its location */
    if (scrmfs_use_memfs) {
        /* allocate a new chunk from memory */
        scrmfs_stack_lock();
        int id = scrmfs_stack_pop(free_chunk_stack);
        scrmfs_stack_unlock();

        /* if we got one return, otherwise try spill over */
        if (id >= 0) {
            /* got a chunk from memory */
            chunk_meta->location = CHUNK_LOCATION_MEMFS;
            chunk_meta->id = id;
        } else if (scrmfs_use_spillover) {
            /* shm segment out of space, grab a block from spill-over device */
            debug("getting blocks from spill-over device\n");

            /* TODO: missing lock calls? */
            /* add scrmfs_max_chunks to identify chunk location */
            scrmfs_stack_lock();
            id = scrmfs_stack_pop(free_spillchunk_stack) + scrmfs_max_chunks;
            scrmfs_stack_unlock();
            if (id < scrmfs_max_chunks) {
                debug("spill-over device out of space (%d)\n", id);
                return SCRMFS_ERR_NOSPC;
            }

            /* got one from spill over */
            chunk_meta->location = CHUNK_LOCATION_SPILLOVER;
            chunk_meta->id = id;
        } else {
            /* spill over isn't available, so we're out of space */
            debug("memfs out of space (%d)\n", id);
            return SCRMFS_ERR_NOSPC;
        }
    } else if (scrmfs_use_spillover) {
        /* memory file system is not enabled, but spill over is */

        /* shm segment out of space, grab a block from spill-over device */
        debug("getting blocks from spill-over device \n");

        /* TODO: missing lock calls? */
        /* add scrmfs_max_chunks to identify chunk location */
        scrmfs_stack_lock();
        int id = scrmfs_stack_pop(free_spillchunk_stack) + scrmfs_max_chunks;
        scrmfs_stack_unlock();
        if (id < scrmfs_max_chunks) {
            debug("spill-over device out of space (%d)\n", id);
            return SCRMFS_ERR_NOSPC;
        }

        /* got one from spill over */
        chunk_meta->location = CHUNK_LOCATION_SPILLOVER;
        chunk_meta->id = id;
    }
  #ifdef HAVE_CONTAINER_LIB
    else if (scrmfs_use_containers) {
        /* unknown chunk type */
        debug("chunks not stored in containers\n");
        return SCRMFS_ERR_IO;
    }
  #endif /* HAVE_CONTAINER_LIB */
    else {
        /* don't know how to allocate chunk */
        chunk_meta->location = CHUNK_LOCATION_NULL;
        return SCRMFS_ERR_IO;
    }

    return SCRMFS_SUCCESS;
}

static int scrmfs_chunk_free(int fid, scrmfs_filemeta_t* meta, int chunk_id)
{
    /* get pointer to chunk meta data */
    scrmfs_chunkmeta_t* chunk_meta = &(meta->chunk_meta[chunk_id]);

    /* get physical id of chunk */
    int id = chunk_meta->id;
    debug("free chunk %d from location %d\n", id, chunk_meta->location);

    /* determine location of chunk */
    if (chunk_meta->location == CHUNK_LOCATION_MEMFS) {
        scrmfs_stack_lock();
        scrmfs_stack_push(free_chunk_stack, id);
        scrmfs_stack_unlock();
    } else if (chunk_meta->location == CHUNK_LOCATION_SPILLOVER) {
        /* TODO: free spill over chunk */
    }
  #ifdef HAVE_CONTAINER_LIB
    else if (chunk_meta->location == CHUNK_LOCATION_CONTAINER) {
        /* unknown chunk type */
        debug("chunks not stored in containers\n");
        return SCRMFS_ERR_IO;
    }
  #endif /* HAVE_CONTAINER_LIB */
    else {
        /* unkwown chunk location */
        debug("unknown chunk location %d\n", chunk_meta->location);
        return SCRMFS_ERR_IO;
    }

    /* update location of chunk */
    chunk_meta->location = CHUNK_LOCATION_NULL;

    return SCRMFS_SUCCESS;
}

/* read data from specified chunk id, chunk offset, and count into user buffer,
 * count should fit within chunk starting from specified offset */
static int scrmfs_chunk_read(
  scrmfs_filemeta_t* meta, /* pointer to file meta data */
  int chunk_id,            /* logical chunk id to read data from */
  off_t chunk_offset,      /* logical offset within chunk to read from */
  void* buf,               /* buffer to store data to */
  size_t count)            /* number of bytes to read */
{
    /* get chunk meta data */
    scrmfs_chunkmeta_t* chunk_meta = &(meta->chunk_meta[chunk_id]);

    /* determine location of chunk */
    if (chunk_meta->location == CHUNK_LOCATION_MEMFS) {
        /* just need a memcpy to read data */
        void* chunk_buf = scrmfs_compute_chunk_buf(meta, chunk_id, chunk_offset);
        memcpy(buf, chunk_buf, count);
    } else if (chunk_meta->location == CHUNK_LOCATION_SPILLOVER) {
        /* spill over to a file, so read from file descriptor */
        //MAP_OR_FAIL(pread);
        off_t spill_offset = scrmfs_compute_spill_offset(meta, chunk_id, chunk_offset);
        ssize_t rc = pread(scrmfs_spilloverblock, buf, count, spill_offset);
        /* TODO: check return code for errors */
    }
  #ifdef HAVE_CONTAINER_LIB
    else if (chunk_meta->location == CHUNK_LOCATION_CONTAINER) {
        /* unknown chunk type */
        debug("chunks not stored in containers\n");
        return SCRMFS_ERR_IO;
    }
  #endif /* HAVE_CONTAINER_LIB */
    else {
        /* unknown chunk type */
        debug("unknown chunk type in read\n");
        return SCRMFS_ERR_IO;
    }

    /* assume read was successful if we get to here */
    return SCRMFS_SUCCESS;
}

/* read data from specified chunk id, chunk offset, and count into user buffer,
 * count should fit within chunk starting from specified offset */
static int scrmfs_chunk_write(
  scrmfs_filemeta_t* meta, /* pointer to file meta data */
  int chunk_id,            /* logical chunk id to write to */
  off_t chunk_offset,      /* logical offset within chunk to write to */
  const void* buf,         /* buffer holding data to be written */
  size_t count)            /* number of bytes to write */
{
    /* get chunk meta data */
    scrmfs_chunkmeta_t* chunk_meta = &(meta->chunk_meta[chunk_id]);

    /* determine location of chunk */
    if (chunk_meta->location == CHUNK_LOCATION_MEMFS) {
        /* just need a memcpy to write data */
        void* chunk_buf = scrmfs_compute_chunk_buf(meta, chunk_id, chunk_offset);
        memcpy(chunk_buf, buf, count);
//        _intel_fast_memcpy(chunk_buf, buf, count);
//        scrmfs_memcpy(chunk_buf, buf, count);
    } else if (chunk_meta->location == CHUNK_LOCATION_SPILLOVER) {
        /* spill over to a file, so write to file descriptor */
        //MAP_OR_FAIL(pwrite);
        off_t spill_offset = scrmfs_compute_spill_offset(meta, chunk_id, chunk_offset);
        ssize_t rc = pwrite(scrmfs_spilloverblock, buf, count, spill_offset);
        if (rc < 0)  {
            perror("pwrite failed");
        }
        /* TODO: check return code for errors */
    }
  #ifdef HAVE_CONTAINER_LIB
    else if (chunk_meta->location == CHUNK_LOCATION_CONTAINER) {
        /* unknown chunk type */
        debug("chunks not stored in containers\n");
        return SCRMFS_ERR_IO;
    }
  #endif /* HAVE_CONTAINER_LIB */
    else {
        /* unknown chunk type */
        debug("unknown chunk type in read\n");
        return SCRMFS_ERR_IO;
    }

    /* assume read was successful if we get to here */
    return SCRMFS_SUCCESS;
}

/* ---------------------------------------
 * Operations on file storage
 * --------------------------------------- */

/* if length is greater than reserved space, reserve space up to length */
int scrmfs_fid_store_fixed_extend(int fid, scrmfs_filemeta_t* meta, off_t length)
{
    /* determine whether we need to allocate more chunks */
    off_t maxsize = meta->chunks << scrmfs_chunk_bits;
    if (length > maxsize) {
        /* compute number of additional bytes we need */
        off_t additional = length - maxsize;
        while (additional > 0) {
            /* check that we don't overrun max number of chunks for file */
            if (meta->chunks == scrmfs_max_chunks) {
                debug("failed to allocate chunk\n");
                return SCRMFS_ERR_NOSPC;
            }

            /* allocate a new chunk */
            int rc = scrmfs_chunk_alloc(fid, meta, meta->chunks);
            if (rc != SCRMFS_SUCCESS) {
                debug("failed to allocate chunk\n");
                return SCRMFS_ERR_NOSPC;
            }

            /* increase chunk count and subtract bytes from the number we need */
            meta->chunks++;
            additional -= scrmfs_chunk_size;
        }
    }

    return SCRMFS_SUCCESS;
}

/* if length is shorter than reserved space, give back space down to length */
int scrmfs_fid_store_fixed_shrink(int fid, scrmfs_filemeta_t* meta, off_t length)
{
    /* determine the number of chunks to leave after truncating */
    off_t num_chunks = 0;
    if (length > 0) {
        num_chunks = (length >> scrmfs_chunk_bits) + 1;
    }

    /* clear off any extra chunks */
    while (meta->chunks > num_chunks) {
        meta->chunks--;
        scrmfs_chunk_free(fid, meta, meta->chunks);
    }

    return SCRMFS_SUCCESS;
}

/* read data from file stored as fixed-size chunks */
int scrmfs_fid_store_fixed_read(int fid, scrmfs_filemeta_t* meta, off_t pos, void* buf, size_t count)
{
    int rc;

    /* get pointer to position within first chunk */
    int chunk_id = pos >> scrmfs_chunk_bits;
    off_t chunk_offset = pos & scrmfs_chunk_mask;

    /* determine how many bytes remain in the current chunk */
    size_t remaining = scrmfs_chunk_size - chunk_offset;
    if (count <= remaining) {
        /* all bytes for this read fit within the current chunk */
        rc = scrmfs_chunk_read(meta, chunk_id, chunk_offset, buf, count);
    } else {
        /* read what's left of current chunk */
        char* ptr = (char*) buf;
        rc = scrmfs_chunk_read(meta, chunk_id, chunk_offset, (void*)ptr, remaining);
        ptr += remaining;
   
        /* read from the next chunk */
        size_t processed = remaining;
        while (processed < count && rc == SCRMFS_SUCCESS) {
            /* get pointer to start of next chunk */
            chunk_id++;

            /* compute size to read from this chunk */
            size_t num = count - processed;
            if (num > scrmfs_chunk_size) {
                num = scrmfs_chunk_size;
            }
   
            /* read data */
            rc = scrmfs_chunk_read(meta, chunk_id, 0, (void*)ptr, num);
            ptr += num;

            /* update number of bytes written */
            processed += num;
        }
    }

    return rc;
}

/* write data to file stored as fixed-size chunks */
int scrmfs_fid_store_fixed_write(int fid, scrmfs_filemeta_t* meta, off_t pos, const void* buf, size_t count)
{
    int rc;

    /* get pointer to position within first chunk */
    int chunk_id = pos >> scrmfs_chunk_bits;
    off_t chunk_offset = pos & scrmfs_chunk_mask;

    /* determine how many bytes remain in the current chunk */
    size_t remaining = scrmfs_chunk_size - chunk_offset;
    if (count <= remaining) {
        /* all bytes for this write fit within the current chunk */
        rc = scrmfs_chunk_write(meta, chunk_id, chunk_offset, buf, count);
    } else {
        /* otherwise, fill up the remainder of the current chunk */
        char* ptr = (char*) buf;
        rc = scrmfs_chunk_write(meta, chunk_id, chunk_offset, (void*)ptr, remaining);
        ptr += remaining;

        /* then write the rest of the bytes starting from beginning
         * of chunks */
        size_t processed = remaining;
        while (processed < count && rc == SCRMFS_SUCCESS) {
            /* get pointer to start of next chunk */
            chunk_id++;

            /* compute size to write to this chunk */
            size_t num = count - processed;
            if (num > scrmfs_chunk_size) {
              num = scrmfs_chunk_size;
            }
   
            /* write data */
            rc = scrmfs_chunk_write(meta, chunk_id, 0, (void*)ptr, num);
            ptr += num;

            /* update number of bytes processed */
            processed += num;
        }
    }

    return rc;
}