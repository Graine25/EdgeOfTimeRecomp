import struct
import sys

BITRATES = {
    (1, 3): [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0],
    (2, 3): [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0],
}
SAMPLE_RATES = {1: [44100, 48000, 32000, 0], 2: [22050, 24000, 16000, 0], 25: [11025, 12000, 8000, 0]}


def frames(data):
    pos = 0
    if data[:3] == b"ID3":
        size = 0
        for b in data[6:10]:
            size = (size << 7) | (b & 0x7F)
        pos = 10 + size
    while pos + 4 <= len(data):
        header = struct.unpack(">I", data[pos:pos + 4])[0]
        if (header >> 21) != 0x7FF:
            pos += 1
            continue
        version_bits = (header >> 19) & 3
        layer_bits = (header >> 17) & 3
        bitrate_index = (header >> 12) & 15
        rate_index = (header >> 10) & 3
        padding = (header >> 9) & 1
        version = {3: 1, 2: 2, 0: 25}.get(version_bits)
        layer = {1: 3, 2: 2, 3: 1}.get(layer_bits)
        if version is None or layer != 3 or bitrate_index in (0, 15) or rate_index == 3:
            pos += 1
            continue
        kbps = BITRATES[(1 if version == 1 else 2, 3)][bitrate_index]
        rate = SAMPLE_RATES[version][rate_index]
        samples = 1152 if version == 1 else 576
        length = (samples // 8) * kbps * 1000 // rate + padding
        if length < 4 or pos + length > len(data):
            break
        yield pos, length, samples / rate
        pos += length


def is_info_frame(data, offset, length):
    body = data[offset:offset + length]
    return b"Xing" in body or b"Info" in body


def main():
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    source, out, start = sys.argv[1], sys.argv[2], float(sys.argv[3])
    end = float(sys.argv[4]) if len(sys.argv) == 5 else None
    data = open(source, "rb").read()

    kept, total, at, dropped_info = [], 0.0, 0.0, False
    for offset, length, seconds in frames(data):
        if not kept and not dropped_info and is_info_frame(data, offset, length):
            dropped_info = True
            continue
        if at >= start and (end is None or at < end):
            kept.append(data[offset:offset + length])
            total += seconds
        at += seconds

    if not kept:
        sys.exit(f"{source}: nothing after {start:.1f} s (the file is {at:.1f} s long)")
    with open(out, "wb") as f:
        for frame in kept:
            f.write(frame)
    print(f"{source}: {at / 60:.0f}:{at % 60:04.1f} long; wrote {out}: {len(kept)} frames, "
          f"{total / 60:.0f}:{total % 60:04.1f}, {sum(map(len, kept)) / 1024:.0f} KiB")


if __name__ == "__main__":
    main()
