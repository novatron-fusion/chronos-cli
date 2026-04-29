/****************************************************************************
 *  Copyright (C) 2026 Novatron Fusion Group.                                *
 *                                                                          *
 *  This program is free software: you can redistribute it and/or modify    *
 *  it under the terms of the GNU General Public License as published by    *
 *  the Free Software Foundation, either version 3 of the License, or       *
 *  (at your option) any later version.                                     *
 ****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <inttypes.h>
#include <hdf5.h>

#include "h5.h"
#include "bitshuffle.h"
#include "bitshuffle_core.h"

/* Bitshuffle HDF5 filter ID (registered third-party filter). */
#define BSHUF_H5FILTER       32008
#define BSHUF_H5_COMPRESS_LZ4 2
#define BSHUF_FILTER_HDR_LEN  12   /* 8-byte uncomp size + 4-byte block size */

struct h5_sink {
    hid_t    file;
    hid_t    dset;
    hid_t    dspace;     /* file dataspace (kept for dataset creation) */
    uint32_t hres;
    uint32_t vres;
    uint32_t nframes;
    uint32_t frame_idx;
    int      errored;
    char    *path;       /* strdup'd copy, for unlink on error */
    void    *comp_buf;   /* scratch buffer for bitshuffle+LZ4 output */
    size_t   comp_buf_sz;
};

static int
h5_write_scalar_attr(hid_t loc, const char *name, hid_t type, const void *value)
{
    hid_t space, attr;
    herr_t rc;

    space = H5Screate(H5S_SCALAR);
    if (space < 0) return -1;
    attr = H5Acreate2(loc, name, type, space, H5P_DEFAULT, H5P_DEFAULT);
    H5Sclose(space);
    if (attr < 0) return -1;
    rc = H5Awrite(attr, type, value);
    H5Aclose(attr);
    return (rc < 0) ? -1 : 0;
}

static int
h5_write_u32_array_attr(hid_t loc, const char *name,
                        const uint32_t *values, size_t n)
{
    hsize_t dims[1];
    hid_t space, attr;
    herr_t rc;

    dims[0] = n;
    space = H5Screate_simple(1, dims, NULL);
    if (space < 0) return -1;
    attr = H5Acreate2(loc, name, H5T_NATIVE_UINT32, space, H5P_DEFAULT, H5P_DEFAULT);
    H5Sclose(space);
    if (attr < 0) return -1;
    rc = H5Awrite(attr, H5T_NATIVE_UINT32, values);
    H5Aclose(attr);
    return (rc < 0) ? -1 : 0;
}

static int
h5_write_string_attr(hid_t loc, const char *name, const char *value)
{
    hid_t type, space, attr;
    size_t len;
    herr_t rc;

    type = H5Tcopy(H5T_C_S1);
    if (type < 0) return -1;
    len = strlen(value);
    if (H5Tset_size(type, len ? len : 1) < 0) {
        H5Tclose(type);
        return -1;
    }
    space = H5Screate(H5S_SCALAR);
    if (space < 0) {
        H5Tclose(type);
        return -1;
    }
    attr = H5Acreate2(loc, name, type, space, H5P_DEFAULT, H5P_DEFAULT);
    H5Sclose(space);
    if (attr < 0) {
        H5Tclose(type);
        return -1;
    }
    rc = H5Awrite(attr, type, value);
    H5Aclose(attr);
    H5Tclose(type);
    return (rc < 0) ? -1 : 0;
}

h5_sink_t *
h5_sink_open(const char *path, uint32_t hres, uint32_t vres, uint32_t nframes,
             const h5_attrs_t *attrs)
{
    h5_sink_t *s;
    hid_t plist = -1;
    hsize_t dims[3];
    hsize_t chunk[3];
    uint32_t res[2];
    size_t n_elem;

    if (!path || hres == 0 || vres == 0 || nframes == 0) {
        fprintf(stderr, "h5_sink_open: invalid arguments\n");
        return NULL;
    }

    s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->file = -1;
    s->dset = -1;
    s->dspace = -1;
    s->hres = hres;
    s->vres = vres;
    s->nframes = nframes;
    s->path = strdup(path);
    if (!s->path) goto fail;

    /* Allocate compression scratch buffer (+ 12 bytes for the bitshuffle
     * filter header that readers expect). */
    n_elem = (size_t)hres * vres;
    s->comp_buf_sz = bshuf_compress_lz4_bound(n_elem, sizeof(uint16_t), 0)
                     + BSHUF_FILTER_HDR_LEN;
    s->comp_buf = malloc(s->comp_buf_sz);
    if (!s->comp_buf) goto fail;

    s->file = H5Fcreate(path, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (s->file < 0) {
        fprintf(stderr, "h5_sink_open: H5Fcreate(%s) failed\n", path);
        goto fail;
    }

    dims[0] = nframes; dims[1] = vres; dims[2] = hres;
    s->dspace = H5Screate_simple(3, dims, NULL);
    if (s->dspace < 0) goto fail;

    plist = H5Pcreate(H5P_DATASET_CREATE);
    if (plist < 0) goto fail;
    chunk[0] = 1; chunk[1] = vres; chunk[2] = hres;
    if (H5Pset_chunk(plist, 3, chunk) < 0) goto fail;

    /* Declare the bitshuffle+LZ4 filter (ID 32008) in the dataset metadata.
     * We write pre-compressed chunks via H5Dwrite_chunk, bypassing the
     * filter pipeline, so the filter's set_local callback never runs.
     * We must store the *expanded* cd_values that readers expect:
     *   [0] = BSHUF_VERSION_MAJOR  (0)
     *   [1] = BSHUF_VERSION_MINOR  (4)
     *   [2] = element size in bytes (2 for uint16)
     *   [3] = block size (0 = auto)
     *   [4] = compression type (2 = LZ4) */
    {
        unsigned cd_values[5] = { 0, 4, sizeof(uint16_t), 0,
                                  BSHUF_H5_COMPRESS_LZ4 };
        if (H5Pset_filter(plist, BSHUF_H5FILTER, H5Z_FLAG_OPTIONAL,
                          5, cd_values) < 0) goto fail;
    }

    s->dset = H5Dcreate2(s->file, "/frames", H5T_NATIVE_UINT16, s->dspace,
                         H5P_DEFAULT, plist, H5P_DEFAULT);
    H5Pclose(plist); plist = -1;
    if (s->dset < 0) {
        fprintf(stderr, "h5_sink_open: H5Dcreate2(/frames) failed\n");
        goto fail;
    }

    /* Always-present attrs. */
    res[0] = hres; res[1] = vres;
    h5_write_u32_array_attr(s->dset, "resolution", res, 2);
    {
        int64_t now = (int64_t)time(NULL);
        h5_write_scalar_attr(s->dset, "created_utc", H5T_NATIVE_INT64, &now);
    }

    /* Optional typed attrs -- write only if caller supplied a nonzero value. */
    if (attrs) {
        if (attrs->frame_rate_hz != 0.0) {
            h5_write_scalar_attr(s->dset, "frame_rate_hz",
                                 H5T_NATIVE_DOUBLE, &attrs->frame_rate_hz);
        }
        if (attrs->exposure_us != 0) {
            h5_write_scalar_attr(s->dset, "exposure_us",
                                 H5T_NATIVE_UINT32, &attrs->exposure_us);
        }
        if (attrs->sensor_bit_depth != 0) {
            h5_write_scalar_attr(s->dset, "sensor_bit_depth",
                                 H5T_NATIVE_UINT32, &attrs->sensor_bit_depth);
        }
        /* Extras on the root group as string attrs. */
        if (attrs->extras) {
            hid_t root = H5Gopen2(s->file, "/", H5P_DEFAULT);
            if (root >= 0) {
                const char * const *p = attrs->extras;
                while (p[0] && p[1]) {
                    h5_write_string_attr(root, p[0], p[1]);
                    p += 2;
                }
                H5Gclose(root);
            }
        }
    }

    return s;

fail:
    if (plist >= 0) H5Pclose(plist);
    if (s) {
        if (s->dspace >= 0) H5Sclose(s->dspace);
        if (s->dset   >= 0) H5Dclose(s->dset);
        if (s->file   >= 0) {
            H5Fclose(s->file);
            if (s->path) unlink(s->path);
        }
        free(s->comp_buf);
        free(s->path);
        free(s);
    }
    return NULL;
}

uint32_t
h5_sink_frame_idx(const h5_sink_t *sink)
{
    return sink ? sink->frame_idx : 0;
}

int
h5_sink_write_frame(h5_sink_t *sink, const uint16_t *pixels)
{
    size_t n_elem;
    int64_t comp_sz;
    hsize_t offset[3];

    if (!sink || !pixels) return -1;
    if (sink->frame_idx >= sink->nframes) {
        fprintf(stderr, "h5_sink_write_frame: frame %u past end (%u)\n",
                sink->frame_idx, sink->nframes);
        sink->errored = 1;
        return -1;
    }

    /* Bitshuffle + LZ4 compress the frame.
     * The bitshuffle HDF5 filter expects a 12-byte header before the
     * compressed payload (see bshuf_h5filter.c in the bitshuffle repo):
     *   bytes  0-7: uint64 BE  — uncompressed data size in bytes
     *   bytes  8-11: uint32 BE — block_size * elem_size in bytes
     * We write this header so that readers using the standard filter
     * plugin (h5py + hdf5plugin, h5web, etc.) can decompress. */
    n_elem = (size_t)sink->hres * sink->vres;
    {
        uint8_t *hdr = (uint8_t *)sink->comp_buf;
        uint64_t uncomp_bytes = n_elem * sizeof(uint16_t);
        size_t blk = bshuf_default_block_size(sizeof(uint16_t));
        uint32_t blk_bytes = (uint32_t)(blk * sizeof(uint16_t));

        /* Write 8-byte big-endian uncompressed size. */
        hdr[0] = (uint8_t)(uncomp_bytes >> 56);
        hdr[1] = (uint8_t)(uncomp_bytes >> 48);
        hdr[2] = (uint8_t)(uncomp_bytes >> 40);
        hdr[3] = (uint8_t)(uncomp_bytes >> 32);
        hdr[4] = (uint8_t)(uncomp_bytes >> 24);
        hdr[5] = (uint8_t)(uncomp_bytes >> 16);
        hdr[6] = (uint8_t)(uncomp_bytes >> 8);
        hdr[7] = (uint8_t)(uncomp_bytes);
        /* Write 4-byte big-endian block size in bytes. */
        hdr[8]  = (uint8_t)(blk_bytes >> 24);
        hdr[9]  = (uint8_t)(blk_bytes >> 16);
        hdr[10] = (uint8_t)(blk_bytes >> 8);
        hdr[11] = (uint8_t)(blk_bytes);
    }
    comp_sz = bshuf_compress_lz4(pixels,
                                 (char *)sink->comp_buf + BSHUF_FILTER_HDR_LEN,
                                 n_elem, sizeof(uint16_t), 0);
    if (comp_sz < 0) {
        fprintf(stderr, "h5_sink_write_frame: bshuf_compress_lz4 failed "
                "(err %" PRId64 ") at frame %u\n", comp_sz, sink->frame_idx);
        sink->errored = 1;
        return -1;
    }
    comp_sz += BSHUF_FILTER_HDR_LEN;

    /* Write the pre-compressed chunk directly, bypassing the HDF5 pipeline.
     * filter_mask = 0 means "all declared filters were applied". */
    offset[0] = sink->frame_idx; offset[1] = 0; offset[2] = 0;
    if (H5Dwrite_chunk(sink->dset, H5P_DEFAULT, 0, offset,
                       (size_t)comp_sz, sink->comp_buf) < 0) {
        fprintf(stderr, "h5_sink_write_frame: H5Dwrite_chunk failed at frame %u\n",
                sink->frame_idx);
        sink->errored = 1;
        return -1;
    }
    sink->frame_idx++;
    return 0;
}

int
h5_sink_close(h5_sink_t *sink, int errored)
{
    int rc = 0;

    if (!sink) return 0;
    if (errored) sink->errored = 1;

    if (sink->dspace >= 0 && H5Sclose(sink->dspace) < 0) rc = -1;
    if (sink->dset   >= 0 && H5Dclose(sink->dset)   < 0) rc = -1;
    if (sink->file   >= 0) {
        if (H5Fflush(sink->file, H5F_SCOPE_GLOBAL) < 0) rc = -1;
        if (H5Fclose(sink->file) < 0) rc = -1;
    }

    if (sink->errored && sink->path) {
        unlink(sink->path);
    }
    free(sink->comp_buf);
    free(sink->path);
    free(sink);
    return rc;
}
