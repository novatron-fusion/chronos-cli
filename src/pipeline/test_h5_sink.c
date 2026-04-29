/****************************************************************************
 *  Copyright (C) 2026 Novatron Fusion Group.                                *
 *                                                                          *
 *  Host-side Tier-1 test for the h5 sink. Writes a deterministic pattern   *
 *  into /tmp/test_h5_sink.h5 with bitshuffle+LZ4 compression, then reads  *
 *  back the raw chunks and verifies decompression.                         *
 *  Links against libhdf5 + bitshuffle + LZ4 -- no GLib, no GStreamer.      *
 ****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdint.h>
#include <inttypes.h>

#include <hdf5.h>

#include "h5.h"
#include "bitshuffle.h"
#include "bshuf_h5filter.h"

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

static int
test_readback_verify(void)
{
    /* Re-open the file written by test_happy_path and verify every frame
     * by reading the raw (compressed) chunk and decompressing it. */
    hid_t file, dset;
    uint16_t *decompressed = NULL;
    void *comp_buf = NULL;
    uint16_t *expected = NULL;
    size_t n_elem = (size_t)TEST_HRES * TEST_VRES;
    size_t frame_bytes = n_elem * sizeof(uint16_t);
    size_t comp_bound;
    uint32_t i;
    int ret = -1;

    comp_bound = bshuf_compress_lz4_bound(n_elem, sizeof(uint16_t), 0) + 12;
    comp_buf     = malloc(comp_bound);
    decompressed = malloc(frame_bytes);
    expected     = malloc(frame_bytes);
    if (!comp_buf || !decompressed || !expected) {
        fprintf(stderr, "test_readback_verify: malloc failed\n");
        goto out;
    }

    file = H5Fopen(TEST_PATH, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file < 0) {
        fprintf(stderr, "test_readback_verify: H5Fopen(%s) failed\n", TEST_PATH);
        goto out;
    }
    dset = H5Dopen2(file, "/frames", H5P_DEFAULT);
    if (dset < 0) {
        fprintf(stderr, "test_readback_verify: H5Dopen2 failed\n");
        H5Fclose(file);
        goto out;
    }

    for (i = 0; i < TEST_NFRAMES; i++) {
        hsize_t offset[3] = { i, 0, 0 };
        uint32_t filter_mask = 0;
        hsize_t chunk_nbytes = 0;

        /* Query the stored (compressed) chunk size. */
        if (H5Dget_chunk_storage_size(dset, offset, &chunk_nbytes) < 0 ||
            chunk_nbytes == 0) {
            fprintf(stderr, "test_readback_verify: frame %u: "
                    "get_chunk_storage_size failed\n", i);
            H5Dclose(dset); H5Fclose(file);
            goto out;
        }

        /* Read the raw compressed chunk. */
        if (H5Dread_chunk(dset, H5P_DEFAULT, offset,
                          &filter_mask, comp_buf) < 0) {
            fprintf(stderr, "test_readback_verify: frame %u: "
                    "H5Dread_chunk failed\n", i);
            H5Dclose(dset); H5Fclose(file);
            goto out;
        }

        /* Decompress (skip the 12-byte bitshuffle filter header). */
        {
            int64_t dec_sz = bshuf_decompress_lz4(
                    (const char *)comp_buf + 12, decompressed,
                    n_elem, sizeof(uint16_t), 0);
            if (dec_sz < 0) {
                fprintf(stderr, "test_readback_verify: frame %u: "
                        "decompress failed (err %" PRId64 ")\n", i, dec_sz);
                H5Dclose(dset); H5Fclose(file);
                goto out;
            }
        }

        /* Compare against expected pattern. */
        fill_frame(expected, TEST_HRES, TEST_VRES, i);
        if (memcmp(decompressed, expected, frame_bytes) != 0) {
            fprintf(stderr, "test_readback_verify: frame %u: "
                    "data mismatch\n", i);
            H5Dclose(dset); H5Fclose(file);
            goto out;
        }
    }

    H5Dclose(dset);
    H5Fclose(file);
    printf("test_readback_verify: all %d frames decompressed and verified\n",
           TEST_NFRAMES);
    ret = 0;

out:
    free(comp_buf);
    free(decompressed);
    free(expected);
    return ret;
}

static int
test_h5dread_roundtrip(void)
{
    /* Read the file written by test_happy_path via H5Dread, exactly as a
     * user with the bitshuffle filter registered would. This exercises the
     * full HDF5 filter pipeline decompression path. */
    hid_t file, dset, fspace, mspace;
    uint16_t *frame = NULL;
    uint16_t *expected = NULL;
    size_t frame_bytes = (size_t)TEST_HRES * TEST_VRES * sizeof(uint16_t);
    hsize_t start[3], count[3], mdims[3];
    uint32_t i;
    int ret = -1;

    /* Register the bitshuffle filter so H5Dread can decompress. */
    if (bshuf_register_h5filter() < 0) {
        fprintf(stderr, "test_h5dread_roundtrip: bshuf_register_h5filter failed\n");
        return -1;
    }

    frame    = malloc(frame_bytes);
    expected = malloc(frame_bytes);
    if (!frame || !expected) {
        fprintf(stderr, "test_h5dread_roundtrip: malloc failed\n");
        goto out;
    }

    file = H5Fopen(TEST_PATH, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file < 0) {
        fprintf(stderr, "test_h5dread_roundtrip: H5Fopen failed\n");
        goto out;
    }
    dset = H5Dopen2(file, "/frames", H5P_DEFAULT);
    if (dset < 0) {
        fprintf(stderr, "test_h5dread_roundtrip: H5Dopen2 failed\n");
        H5Fclose(file);
        goto out;
    }
    fspace = H5Dget_space(dset);

    mdims[0] = 1; mdims[1] = TEST_VRES; mdims[2] = TEST_HRES;
    mspace = H5Screate_simple(3, mdims, NULL);

    for (i = 0; i < TEST_NFRAMES; i++) {
        start[0] = i; start[1] = 0; start[2] = 0;
        count[0] = 1; count[1] = TEST_VRES; count[2] = TEST_HRES;
        H5Sselect_hyperslab(fspace, H5S_SELECT_SET, start, NULL, count, NULL);

        if (H5Dread(dset, H5T_NATIVE_UINT16, mspace, fspace,
                    H5P_DEFAULT, frame) < 0) {
            fprintf(stderr, "test_h5dread_roundtrip: H5Dread failed at frame %u\n", i);
            H5Sclose(mspace); H5Sclose(fspace);
            H5Dclose(dset); H5Fclose(file);
            goto out;
        }

        fill_frame(expected, TEST_HRES, TEST_VRES, i);
        if (memcmp(frame, expected, frame_bytes) != 0) {
            fprintf(stderr, "test_h5dread_roundtrip: data mismatch at frame %u\n", i);
            H5Sclose(mspace); H5Sclose(fspace);
            H5Dclose(dset); H5Fclose(file);
            goto out;
        }
    }

    H5Sclose(mspace);
    H5Sclose(fspace);
    H5Dclose(dset);
    H5Fclose(file);
    printf("test_h5dread_roundtrip: all %d frames read via H5Dread and verified\n",
           TEST_NFRAMES);
    ret = 0;

out:
    free(frame);
    free(expected);
    return ret;
}

int
main(void)
{
    int rc = 0;
    if (test_happy_path() < 0) rc = 1;
    if (test_readback_verify() < 0) rc = 1;
    if (test_h5dread_roundtrip() < 0) rc = 1;
    if (test_overflow_rejected() < 0) rc = 1;
    if (test_forced_errored_close() < 0) rc = 1;
    if (rc == 0) printf("ALL TESTS PASSED\n");
    return rc;
}
