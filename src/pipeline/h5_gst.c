/****************************************************************************
 *  Copyright (C) 2026 Novatron Fusion Group.                                *
 *                                                                          *
 *  This program is free software: you can redistribute it and/or modify    *
 *  it under the terms of the GNU General Public License as published by    *
 *  the Free Software Foundation, either version 3 of the License, or       *
 *  (at your option) any later version.                                     *
 ****************************************************************************/
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <glib.h>
#include <gst/gst.h>

#include "pipeline.h"
#include "h5.h"

/* Keys already consumed by cam_video_recordfile -- don't forward them as
 * per-shot attribute extras. */
static int
h5_is_reserved_key(const char *name)
{
    return !strcmp(name, "format")    || !strcmp(name, "filename") ||
           !strcmp(name, "start")     || !strcmp(name, "length")   ||
           !strcmp(name, "framerate") || !strcmp(name, "bitrate");
}

/* Collect every *string-valued* key from the recordfile args dict (other than
 * the reserved control keys) into a freshly-allocated NULL-terminated array
 * of alternating {key, value} pairs: [k0,v0,k1,v1,...,NULL]. */
char **
h5_extras_from_dict(GHashTable *args)
{
    GHashTableIter iter;
    gpointer key, value;
    char **out;
    size_t cap, n = 0;

    if (!args) return NULL;
    cap = g_hash_table_size(args);
    out = calloc(cap * 2 + 2, sizeof(char *));
    if (!out) return NULL;

    g_hash_table_iter_init(&iter, args);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        const char *name = key;
        GValue *gval = value;
        if (h5_is_reserved_key(name)) continue;
        if (!gval || !G_VALUE_HOLDS_STRING(gval)) continue;
        out[n * 2]     = strdup(name);
        out[n * 2 + 1] = strdup(g_value_get_string(gval));
        if (!out[n * 2] || !out[n * 2 + 1]) {
            /* Bail -- caller will free the partial array. */
            free(out[n * 2]);
            free(out[n * 2 + 1]);
            break;
        }
        n++;
    }
    out[n * 2]     = NULL;
    out[n * 2 + 1] = NULL;
    return out;
}

void
h5_free_extras(char **extras)
{
    char **p;
    if (!extras) return;
    for (p = extras; *p; p++) free(*p);
    free(extras);
}

static gboolean
h5_probe(GstPad *pad, GstBuffer *buffer, gpointer cbdata)
{
    struct pipeline_state *state = cbdata;
    size_t expected;

    (void)pad;
    if (!state->h5_sink) return TRUE;

    expected = (size_t)state->source.hres * (size_t)state->source.vres * 2;
    if (GST_BUFFER_SIZE(buffer) < expected) {
        fprintf(stderr, "h5_probe: buffer size %u < expected %zu\n",
                (unsigned)GST_BUFFER_SIZE(buffer), expected);
        snprintf(state->error, sizeof(state->error), "h5 buffer undersized");
        return TRUE;
    }
    if (h5_sink_write_frame(state->h5_sink,
                            (const uint16_t *)GST_BUFFER_DATA(buffer)) < 0) {
        snprintf(state->error, sizeof(state->error), "h5 write failed");
    }
    return TRUE;
}

GstPad *
cam_h5_sink(struct pipeline_state *state, struct pipeline_args *args)
{
    GstElement *queue, *sink;
    GstPad *pad;
    h5_attrs_t attrs;

    /* Build attrs from what the pipeline knows. Framerate is whatever the
     * caller passed in the recordfile args dict (0 if not supplied). Sensor
     * bit depth for the Chronos LUX1310/LUX2100 is fixed at 12 bits padded
     * into 16. Exposure is not plumbed through for MVP -- callers who want
     * it should supply it as a string extra. */
    memset(&attrs, 0, sizeof(attrs));
    attrs.frame_rate_hz    = (double)args->framerate;
    attrs.exposure_us      = 0;
    attrs.sensor_bit_depth = 12;
    attrs.extras           = (const char * const *)state->h5_extras;

    state->h5_sink = h5_sink_open(args->filename,
                                  state->source.hres, state->source.vres,
                                  args->length, &attrs);
    if (!state->h5_sink) {
        snprintf(state->error, sizeof(state->error),
                 "h5_sink_open(%s) failed", args->filename);
        return NULL;
    }

    queue = gst_element_factory_make("queue",    "h5-queue");
    sink  = gst_element_factory_make("fakesink", "h5-sink");
    if (!queue || !sink) {
        h5_sink_close(state->h5_sink, 1);
        state->h5_sink = NULL;
        return NULL;
    }

    pad = gst_element_get_static_pad(queue, "src");
    gst_pad_add_buffer_probe(pad, G_CALLBACK(h5_probe), state);
    gst_object_unref(pad);

    gst_bin_add_many(GST_BIN(state->pipeline), queue, sink, NULL);
    gst_element_link_many(queue, sink, NULL);
    return gst_element_get_static_pad(queue, "sink");
}
