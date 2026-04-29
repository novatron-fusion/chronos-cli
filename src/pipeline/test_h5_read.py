#!/usr/bin/env python3
"""Tier-2 read test: verify a bitshuffle+LZ4 HDF5 file written by test_h5_sink.

Run after ./test_h5_sink so /tmp/test_h5_sink.h5 exists:
    python3 test_h5_read.py

Requires:  pip install h5py hdf5plugin numpy
"""
import sys
import numpy as np
import hdf5plugin          # registers the bitshuffle filter with HDF5
import h5py

PATH = "/tmp/test_h5_sink.h5"
HRES, VRES, NFRAMES = 64, 48, 200


def expected_frame(idx):
    y = np.arange(VRES, dtype=np.uint16)[:, None]
    x = np.arange(HRES, dtype=np.uint16)[None, :]
    return ((idx << 8) | ((y ^ x) & 0xFF)).astype(np.uint16)


def main():
    f = h5py.File(PATH, "r")
    ds = f["frames"]

    assert ds.shape == (NFRAMES, VRES, HRES), f"shape mismatch: {ds.shape}"
    assert ds.dtype == np.uint16, f"dtype mismatch: {ds.dtype}"
    assert ds.chunks == (1, VRES, HRES), f"chunks mismatch: {ds.chunks}"

    # Verify filter metadata.
    dcpl = ds.id.get_create_plist()
    filt = dcpl.get_filter(0)
    filt_id, _flags, cd_values, _name = filt
    assert filt_id == 32008, f"filter id mismatch: {filt_id}"
    assert cd_values[2] == 2, f"elem_size mismatch: {cd_values[2]}"
    assert cd_values[4] == 2, f"compression type mismatch: {cd_values[4]}"

    # Spot-check first, middle, and last frames.
    for i in [0, NFRAMES // 2, NFRAMES - 1]:
        frame = ds[i]
        exp = expected_frame(i)
        if not np.array_equal(frame, exp):
            first_diff = np.argwhere(frame != exp)[0]
            print(f"FAIL frame {i}: mismatch at {tuple(first_diff)}, "
                  f"got {frame[tuple(first_diff)]}, expected {exp[tuple(first_diff)]}")
            f.close()
            return 1

    # Full sequential read.
    for i in range(NFRAMES):
        frame = ds[i]
        exp = expected_frame(i)
        if not np.array_equal(frame, exp):
            print(f"FAIL frame {i}: data mismatch")
            f.close()
            return 1

    f.close()
    print(f"test_h5_read.py: all {NFRAMES} frames verified via h5py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
