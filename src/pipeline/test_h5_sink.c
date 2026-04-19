/****************************************************************************
 *  Copyright (C) 2026 Novatron Fusion Group.                                *
 *                                                                          *
 *  Host-side Tier-1 test for the h5 sink. Writes a deterministic pattern   *
 *  into /tmp/test_h5_sink.h5 so a Python/h5py driver can verify it.         *
 *  Links only against libhdf5 -- no GLib, no GStreamer.                    *
 ****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdint.h>

#include "h5.h"

#define TEST_HRES    64
#define TEST_VRES    48
#define TEST_NFRAMES 200
#define TEST_PATH    "/tmp/test_h5_sink.h5"
#define TEST_ERR_PATH "/tmp/test_h5_sink_err.h5"

static void
fill_frame(uint16_t *pixels, uint32_t hres, uint32_t vres, uint32_t frame_idx)
{
    /* Pattern: pixels[y*hres + x] = (frame_idx << 8) | ((y ^ x) & 0xFF)
     * Distinguishable across frames and spatially. */
    uint32_t y, x;
    for (y = 0; y < vres; y++) {
        for (x = 0; x < hres; x++) {
            pixels[y * hres + x] = (uint16_t)((frame_idx << 8) | ((y ^ x) & 0xFF));
        }
    }
}

static int
test_happy_path(void)
{
    h5_sink_t *sink;
    uint16_t *frame;
    h5_attrs_t attrs;
    const char *extras[] = {
        "shot_id",       "test1",
        "experiment_id", "nfg-n1-2026-04-19",
        NULL
    };
    uint32_t i;

    memset(&attrs, 0, sizeof(attrs));
    attrs.frame_rate_hz    = 1000.0;
    attrs.exposure_us      = 500;
    attrs.sensor_bit_depth = 12;
    attrs.extras           = extras;

    frame = malloc((size_t)TEST_HRES * TEST_VRES * sizeof(uint16_t));
    if (!frame) {
        fprintf(stderr, "test_happy_path: malloc failed\n");
        return -1;
    }

    unlink(TEST_PATH);
    sink = h5_sink_open(TEST_PATH, TEST_HRES, TEST_VRES, TEST_NFRAMES, &attrs);
    if (!sink) {
        fprintf(stderr, "test_happy_path: h5_sink_open failed\n");
        free(frame);
        return -1;
    }

    for (i = 0; i < TEST_NFRAMES; i++) {
        fill_frame(frame, TEST_HRES, TEST_VRES, i);
        if (h5_sink_write_frame(sink, frame) < 0) {
            fprintf(stderr, "test_happy_path: write_frame failed at %u\n", i);
            h5_sink_close(sink, 1);
            free(frame);
            return -1;
        }
    }

    if (h5_sink_close(sink, 0) != 0) {
        fprintf(stderr, "test_happy_path: close returned nonzero\n");
        free(frame);
        return -1;
    }
    free(frame);

    /* Confirm the file still exists after a clean close. */
    {
        struct stat st;
        if (stat(TEST_PATH, &st) != 0) {
            fprintf(stderr, "test_happy_path: %s missing after clean close\n",
                    TEST_PATH);
            return -1;
        }
    }
    printf("test_happy_path: wrote %d frames to %s\n", TEST_NFRAMES, TEST_PATH);
    return 0;
}

static int
test_overflow_rejected(void)
{
    h5_sink_t *sink;
    uint16_t *frame;
    uint32_t i;
    int rc;

    frame = calloc((size_t)TEST_HRES * TEST_VRES, sizeof(uint16_t));
    if (!frame) return -1;

    unlink(TEST_ERR_PATH);
    sink = h5_sink_open(TEST_ERR_PATH, TEST_HRES, TEST_VRES, 2, NULL);
    if (!sink) {
        free(frame);
        return -1;
    }
    for (i = 0; i < 2; i++) {
        if (h5_sink_write_frame(sink, frame) < 0) {
            h5_sink_close(sink, 1);
            free(frame);
            return -1;
        }
    }
    /* Third write past the end -- must return -1 and mark errored. */
    rc = h5_sink_write_frame(sink, frame);
    if (rc == 0) {
        fprintf(stderr, "test_overflow_rejected: write past end succeeded\n");
        h5_sink_close(sink, 1);
        unlink(TEST_ERR_PATH);
        free(frame);
        return -1;
    }
    h5_sink_close(sink, 0); /* sink->errored should trigger unlink */
    free(frame);

    /* File must have been removed. */
    if (access(TEST_ERR_PATH, F_OK) == 0) {
        fprintf(stderr, "test_overflow_rejected: %s still present after "
                "errored close\n", TEST_ERR_PATH);
        unlink(TEST_ERR_PATH);
        return -1;
    }
    printf("test_overflow_rejected: write past end rejected and file removed\n");
    return 0;
}

static int
test_forced_errored_close(void)
{
    h5_sink_t *sink;
    uint16_t *frame;

    frame = calloc((size_t)TEST_HRES * TEST_VRES, sizeof(uint16_t));
    if (!frame) return -1;

    unlink(TEST_ERR_PATH);
    sink = h5_sink_open(TEST_ERR_PATH, TEST_HRES, TEST_VRES, 5, NULL);
    if (!sink) {
        free(frame);
        return -1;
    }
    h5_sink_write_frame(sink, frame);
    h5_sink_close(sink, 1); /* caller-provided errored flag */
    free(frame);

    if (access(TEST_ERR_PATH, F_OK) == 0) {
        fprintf(stderr, "test_forced_errored_close: %s still present\n",
                TEST_ERR_PATH);
        unlink(TEST_ERR_PATH);
        return -1;
    }
    printf("test_forced_errored_close: errored=1 close removed the file\n");
    return 0;
}

int
main(void)
{
    int rc = 0;
    if (test_happy_path() < 0) rc = 1;
    if (test_overflow_rejected() < 0) rc = 1;
    if (test_forced_errored_close() < 0) rc = 1;
    if (rc == 0) printf("ALL TESTS PASSED\n");
    return rc;
}
