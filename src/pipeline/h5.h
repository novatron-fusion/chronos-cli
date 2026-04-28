/****************************************************************************
 *  Copyright (C) 2026 Novatron Fusion Group.                                *
 *                                                                          *
 *  This program is free software: you can redistribute it and/or modify    *
 *  it under the terms of the GNU General Public License as published by    *
 *  the Free Software Foundation, either version 3 of the License, or       *
 *  (at your option) any later version.                                     *
 ****************************************************************************/
#ifndef __PIPELINE_H5_H
#define __PIPELINE_H5_H

#include <stdint.h>

/*
 * HDF5 sink for the camera pipeline. Accepts uint16 frames one at a time and
 * streams them into a chunked /frames dataset (one frame per chunk,
 * uncompressed).
 *
 * This header is deliberately free of GLib / GStreamer / pipeline_state so it
 * can be linked against by a host-side test harness that only needs libhdf5.
 */

typedef struct h5_sink h5_sink_t;

typedef struct {
    double   frame_rate_hz;      /* 0 => don't write this attr */
    uint32_t exposure_us;        /* 0 => don't write this attr */
    uint32_t sensor_bit_depth;   /* 0 => don't write this attr */
    /* NULL-terminated {key, value, key, value, ..., NULL}. May be NULL.
     * Each (key, value) pair is written as a string attribute on the root
     * group. Ownership stays with the caller; strings are copied during open. */
    const char * const *extras;
} h5_attrs_t;

/* Create /path, write an empty /frames dataset shaped (nframes, vres, hres)
 * with chunk (1, vres, hres), uncompressed, plus attrs.
 * Returns NULL on failure and removes any partial file. */
h5_sink_t *h5_sink_open(const char *path,
                        uint32_t hres, uint32_t vres, uint32_t nframes,
                        const h5_attrs_t *attrs);

/* Write one frame's worth of pixels (hres*vres uint16) into the next slot.
 * Returns 0 on success, -1 on failure. After -1 the sink is marked errored
 * and h5_sink_close will unlink the file. */
/* Return the number of frames written so far. */
uint32_t h5_sink_frame_idx(const h5_sink_t *sink);

int h5_sink_write_frame(h5_sink_t *sink, const uint16_t *pixels);

/* Flush and close the file. If errored != 0 or the sink recorded any prior
 * error, the file is unlinked from disk. Frees sink. Returns 0 on clean close. */
int h5_sink_close(h5_sink_t *sink, int errored);

#endif /* __PIPELINE_H5_H */
