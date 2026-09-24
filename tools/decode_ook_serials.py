#!/usr/bin/env python3
"""Find CRC-valid Minimed frames in an OOK HackRF capture.

This is an offline triage decoder, not a substitute for validating packet
timing and separation from transmitter leakage. It uses the known 16.384 kbps
rate, 128-bit preamble, and FF 00 FF 00 sync used by the xG28 captures, then
decodes 4b6b and reports frames by serial/opcode/CRC.

Example:
  python3 tools/decode_ook_serials.py path.cs8 --serial 646910

SPDX-License-Identifier: GPL-2.0-only
"""
import argparse
import sys

import numpy as np

sys.path.insert(0, "/home/charles/ai/orangelink-ncs-ws/orangelink-ncs/tools")
import minimed  # noqa: E402

FS = 2_000_000.0
BITRATE = 16_384.0
OFFSET = 500_000.0
SYNC = "11111111000000001111111100000000"


def packets_from_burst(mag, threshold, samples_per_bit):
    segment = (mag > threshold).astype(np.int8)
    transitions = np.flatnonzero(np.diff(segment) != 0)
    if len(transitions) < 8:
        return []

    best = None
    for phase in np.linspace(0, samples_per_bit, 12, endpoint=False):
        indexes = (transitions[0] + phase + samples_per_bit *
                   np.arange(int((len(segment) - transitions[0] - phase) /
                                 samples_per_bit))).astype(int)
        indexes = indexes[indexes < len(segment)]
        bits = "".join("1" if segment[i] else "0" for i in indexes)
        pos = bits.find(SYNC)
        if pos >= 0 and (best is None or pos < best[0]):
            best = (pos, bits)
    if best is None:
        return []

    pos, bits = best
    payload_bits = bits[pos + len(SYNC):]
    payload_bits = payload_bits[:len(payload_bits) // 8 * 8]
    coded = bytes(int(payload_bits[i:i + 8], 2)
                  for i in range(0, len(payload_bits), 8))
    decoded, _ = minimed.decode_4b6b(coded)

    found = []
    for start, byte in enumerate(decoded):
        if byte != 0xA7 or start + 6 >= len(decoded):
            continue
        # The CRC terminates the Medtronic frame; trailing decoded zero
        # sentinel/noise is allowed, so test possible frame ends.
        for end in range(start + 7, min(len(decoded), start + 128) + 1):
            frame = decoded[start:end]
            if minimed.crc8(frame[:-1]) == frame[-1]:
                found.append(frame)
                break
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture")
    parser.add_argument("--serial", help="report only this six-digit serial")
    parser.add_argument("--all", action="store_true",
                        help="also print CRC-valid frames from other serials")
    parser.add_argument("--min-frame-len", type=int, default=7,
                        help="print frames at least this many bytes long")
    parser.add_argument("--opcode", type=lambda s: int(s, 0),
                        help="print only this opcode, e.g. 0x8d")
    args = parser.parse_args()

    raw = np.fromfile(args.capture, dtype=np.int8).astype(np.float32)
    iq = raw[0::2] + 1j * raw[1::2]
    del raw
    n = np.arange(len(iq), dtype=np.float64)
    iq *= np.exp(-2j * np.pi * (OFFSET / FS) * n).astype(np.complex64)
    decimation = 3
    iq = iq[:len(iq) // decimation * decimation].reshape(-1, decimation).mean(axis=1)
    magnitude = np.abs(iq).astype(np.float32)
    del iq, n

    sample_rate = FS / decimation
    samples_per_bit = sample_rate / BITRATE
    noise = np.median(magnitude)
    threshold = noise + 0.25 * (np.percentile(magnitude, 99.9) - noise)
    smoothing = int(samples_per_bit * 6)
    envelope = (np.convolve((magnitude > threshold).astype(np.float32),
                            np.ones(smoothing) / smoothing, mode="same") > 0.12)
    edges = np.diff(envelope.astype(np.int8))
    starts = list(np.flatnonzero(edges == 1) + 1)
    ends = list(np.flatnonzero(edges == -1) + 1)
    if envelope[0]:
        starts.insert(0, 0)
    if envelope[-1]:
        ends.append(len(envelope))

    bursts = []
    for start, end in zip(starts, ends):
        if bursts and (start - bursts[-1][1]) / sample_rate < 0.003:
            bursts[-1] = (bursts[-1][0], end)
        else:
            bursts.append((start, end))

    expected = minimed.serial_bytes(args.serial) if args.serial else None
    print(f"capture={args.capture} duration={len(magnitude) / sample_rate:.3f}s "
          f"bursts={len(bursts)}")
    count = 0
    for start, end in bursts:
        if (end - start) / sample_rate < 0.004:
            continue
        frames = packets_from_burst(magnitude[start:end], threshold,
                                   samples_per_bit)
        for frame in frames:
            serial = frame[1:4]
            if expected and serial != expected and not args.all:
                continue
            if len(frame) < args.min_frame_len:
                continue
            if args.opcode is not None and frame[4] != args.opcode:
                continue
            count += 1
            print(f"t={start / sample_rate:9.4f}s serial={serial.hex()} "
                  f"op=0x{frame[4]:02x} len={len(frame)} "
                  f"crc=OK frame={frame.hex()}")
    print(f"crc_valid_frames={count}")


if __name__ == "__main__":
    main()
