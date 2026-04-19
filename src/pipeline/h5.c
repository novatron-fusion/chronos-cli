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
#include <hdf5.h>

#include "h5.h"

#define H5_CHUNK_FRAMES 16

struct h5_sink {
    hid_t    file;
    hid_t    dset;
    hid_t    dspace;     /* file dataspace */
    hid_t    mspace;     /* memory dataspace for one frame */
    uint32_t hres;
    uint32_t vres;
    uint32_t nframes;
    uint32_t frame_idx;
    int      errored;
    char    *path;       /* strdup'd copy, for unlink on error */
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
    hsize_t mdims[3];
    uint32_t res[2];

    if (!path || hres == 0 || vres == 0 || nframes == 0) {
        fprintf(stderr, "h5_sink_open: invalid arguments\n");
        return NULL;
    }

    s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->file = -1;
    s->dset = -1;
    s->dspace = -1;
    s->mspace = -1;
    s->hres = hres;
    s->vres = vres;
    s->nframes = nframes;
    s->path = strdup(path);
    if (!s->path) goto fail;

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
    chunk[0] = (nframes < H5_CHUNK_FRAMES) ? nframes : H5_CHUNK_FRAMES;
    chunk[1] = vres; chunk[2] = hres;
    if (H5Pset_chunk(plist, 3, chunk) < 0) goto fail;
    /* Shuffle must be applied BEFORE deflate so consecutive bytes of u16s
     * sit next to each other, giving the compressor something repetitive. */
    if (H5Pset_shuffle(plist) < 0) goto fail;
    if (H5Pset_deflate(plist, 3) < 0) goto fail;

    s->dset = H5Dcreate2(s->file, "/frames", H5T_NATIVE_UINT16, s->dspace,
                         H5P_DEFAULT, plist, H5P_DEFAULT);
    H5Pclose(plist); plist = -1;
    if (s->dset < 0) {
        fprintf(stderr, "h5_sink_open: H5Dcreate2(/frames) failed\n");
        goto fail;
    }

    mdims[0] = 1; mdims[1] = vres; mdims[2] = hres;
    s->mspace = H5Screate_simple(3, mdims, NULL);
    if (s->mspace < 0) goto fail;

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
        if (s->mspace >= 0) H5Sclose(s->mspace);
        if (s->dspace >= 0) H5Sclose(s->dspace);
        if (s->dset   >= 0) H5Dclose(s->dset);
        if (s->file   >= 0) {
            H5Fclose(s->file);
            if (s->path) unlink(s->path);
        }
        free(s->path);
        free(s);
    }
    return NULL;
}

int
h5_sink_write_frame(h5_sink_t *sink, const uint16_t *pixels)
{
    hsize_t start[3];
    hsize_t count[3];

    if (!sink || !pixels) return -1;
    if (sink->frame_idx >= sink->nframes) {
        fprintf(stderr, "h5_sink_write_frame: frame %u past end (%u)\n",
                sink->frame_idx, sink->nframes);
        sink->errored = 1;
        return -1;
    }

    start[0] = sink->frame_idx; start[1] = 0;         start[2] = 0;
    count[0] = 1;               count[1] = sink->vres; count[2] = sink->hres;
    if (H5Sselect_hyperslab(sink->dspace, H5S_SELECT_SET, start, NULL,
                            count, NULL) < 0) {
        sink->errored = 1;
        return -1;
    }
    if (H5Dwrite(sink->dset, H5T_NATIVE_UINT16, sink->mspace, sink->dspace,
                 H5P_DEFAULT, pixels) < 0) {
        fprintf(stderr, "h5_sink_write_frame: H5Dwrite failed at frame %u\n",
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

    if (sink->mspace >= 0 && H5Sclose(sink->mspace) < 0) rc = -1;
    if (sink->dspace >= 0 && H5Sclose(sink->dspace) < 0) rc = -1;
    if (sink->dset   >= 0 && H5Dclose(sink->dset)   < 0) rc = -1;
    if (sink->file   >= 0) {
        if (H5Fflush(sink->file, H5F_SCOPE_GLOBAL) < 0) rc = -1;
        if (H5Fclose(sink->file) < 0) rc = -1;
    }

    if (sink->errored && sink->path) {
        unlink(sink->path);
    }
    free(sink->path);
    free(sink);
    return rc;
}
