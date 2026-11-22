"""Bake the Liquid Light print and clock phases. No graphics library is needed on the MPC."""
import math
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
SIZE = 56
PALETTE_KEYS = 24
EYE_WIDTH, EYE_HEIGHT, EYE_FRAMES = 72, 28, 280
TITLE_SCALE = 4  # horizontal samples per display pixel
STOPS = [
    (0, 31, 10, 70), (.18, 69, 24, 145), (.32, 82, 43, 237), (.42, 32, 192, 249),
    (.47, 85, 252, 207), (.515, 165, 251, 88), (.55, 82, 19, 117), (.64, 220, 34, 170),
    (.74, 255, 60, 84), (.855, 255, 152, 37), (.95, 255, 225, 91), (1, 255, 250, 192),
]


# Each ramp preserves the sharp caustic at the middle of the glass. Its narrow
# reflection carries the complementary ink when the broad petals are warm or cool.
DRIFT_INKS = (
    "200d34 3b1b53 6f3195 7152e9 9787ff d8baff 462055 823c4e f56828 ff8938 ffb963 ffe4b6",
    "190c38 3b2061 663496 9b65f2 b697ff e2cdff 381955 74358c ae51d5 c97efa deb2ff f3ddff",
    "110e36 251b63 4e3297 a4699b f29764 ffc292 27164e 322582 4444cc 576bff 9caeff dbe2ff",
)


def palette(stops):
    result = [0]  # transparent corners
    for n in range(255):
        t = n / 254
        left, right = next((a, b) for a, b in zip(stops, stops[1:]) if t <= b[0])
        f = (t - left[0]) / (right[0] - left[0])
        r, g, b = (round(a + f * (b - a)) for a, b in zip(left[1:], right[1:]))
        result.append(0xff000000 | r << 16 | g << 8 | b)
    return result


def drift_keys():
    ramps = []
    for inks in DRIFT_INKS:
        stops = [(pos, *bytes.fromhex(color)) for (pos, *_), color in zip(STOPS, inks.split())]
        ramps.append(palette(stops))
    for key in range(PALETTE_KEYS):
        colors = [0]
        for i in range(1, 256):
            # Stagger the thin reflection against the petals to give the glass depth.
            lag = .11 * math.sin(math.pi * i / 255) ** 2
            t = 1 - math.cos(math.tau * (key / PALETTE_KEYS + lag))
            side = min(1, int(t))
            f = t - side
            a, b = ramps[side][i], ramps[side + 1][i]
            rgb = [round((a >> n & 255) * (1 - f) + (b >> n & 255) * f) for n in (16, 8, 0)]
            colors.append(0xff000000 | rgb[0] << 16 | rgb[1] << 8 | rgb[2])
        yield colors


def lenses():
    for divisor in range(1, 17):
        axes = [(math.sin(math.tau * i / divisor), -math.cos(math.tau * i / divisor)) for i in range(divisor)]
        frames = [bytearray(SIZE * SIZE) for _ in axes]
        for j in range(SIZE):
            for i in range(SIZE):
                x, y = (2 * i + 1) / SIZE - 1, (2 * j + 1) / SIZE - 1
                r = math.hypot(x, y)
                if r > 1:
                    continue
                a = math.atan2(x, -y)
                rr = r + .045 * math.sin(divisor * a + 1.2 * r) * r
                base = .14 + .075 * math.cos(9.5 * rr) + .18 * math.exp(-18 * r * r)
                base += .06 * math.sin(5 * x - 4 * y) + .08 * math.sin(a * divisor + .4) * math.sin(r * math.pi)
                pools = []
                for sx, sy in axes:
                    along, across = x * sx + y * sy - .58, -x * sy + y * sx
                    wave = across + .045 * math.sin(7 * along)
                    # Closely spaced divisions still need distinct petals on a 56-pixel lens.
                    width = (12 if divisor <= 4 else 23) * max(1, divisor / 7) ** 2
                    pools.append(math.exp(-5.1 * along * along - width * wave * wave))
                base += .65 * sum(pools)
                rim = min(1, (1 - r) * 10)
                for frame, pool in zip(frames, pools):
                    heat = (base + .30 * pool) * rim + .04 * (1 - rim)
                    frame[j * SIZE + i] = 1 + round(254 * max(0, min(1, heat)))
        yield from frames


def write_bytes(out, name, data):
    out.write(f'inline constexpr unsigned char {name}[] =\n')
    for start in range(0, len(data), 32):
        out.write('"' + ''.join(f'\\x{n:02x}' for n in data[start:start + 32]) + '"\n')
    out.write(';\n')


def write_silver_title(out, mask, w, h):
    # Silver Thorn's cool face, violet reflection and warm chrome edge.
    reflections = [
        (-8, (168, 147, 186)), (4, (224, 202, 226)), (10, (248, 240, 248)),
        (15, (195, 172, 210)), (17, (128, 101, 158)), (20, (161, 132, 178)),
        (25, (225, 196, 211)), (30, (201, 187, 220)), (42, (233, 220, 238)),
    ]
    ground, light, dark = (18, 11, 52), (255, 246, 250), (69, 46, 96)
    scale = TITLE_SCALE
    print_rgb = bytearray()
    for y in range(h):
        row = bytearray(3 * w)
        for x in range(w):
            alpha = mask[y*w + x] / 255
            rgb = ground
            if alpha:
                # Sample the local contour to round the strokes into the reflected light.
                dx = mask[y*w + min(w-1, x+scale)] - mask[y*w + max(0, x-scale)]
                dy = mask[min(h-1, y+1)*w + x] - mask[max(0, y-1)*w + x]
                bevel = (.64 * dx + .36 * dy) / 255
                band = y + 2.5 * math.sin(x / (32 * scale))
                (a, low), (b, high) = next((s, t) for s, t in zip(reflections, reflections[1:]) if band <= t[0])
                f = (band - a) / (b - a)
                face = [lo + (hi - lo) * f for lo, hi in zip(low, high)]
                edge = light if bevel >= 0 else dark
                rgb = [round(bg + alpha * (v + abs(bevel) * (e - v) - bg))
                       for bg, v, e in zip(ground, face, edge)]
            row[3*x:3*x+3] = bytes(rgb)
        # Resolve the unbent print to native display pixels.
        for x in range(0, w, scale):
            for channel in range(3):
                start = 3*x + channel
                print_rgb.append((sum(row[start:start + 3*scale:3]) + scale//2) // scale)
    write_bytes(out, 'titlePrint', print_rgb)


def write_menu_eye(out, pixels):
    # Every pose is painted over the same open eye. Save only the moving
    # rectangle of each distinct pose, so a seek or a page change is immediate.
    size = EYE_WIDTH * EYE_HEIGHT * 3
    neutral = pixels[:size]
    poses = {neutral: 0}
    patches = [(0, 0, 0, 0, 0)]
    timeline = []
    packed = bytearray()
    for start in range(0, len(pixels), size):
        frame = pixels[start:start + size]
        if frame not in poses:
            changed = [i // 3 for i in range(0, size, 3) if frame[i:i+3] != neutral[i:i+3]]
            left = min(i % EYE_WIDTH for i in changed)
            right = max(i % EYE_WIDTH for i in changed) + 1
            top, bottom = changed[0] // EYE_WIDTH, changed[-1] // EYE_WIDTH + 1
            poses[frame] = len(patches)
            patches.append((left, top, right - left, bottom - top, len(packed)))
            for y in range(top, bottom):
                row = y * EYE_WIDTH
                packed.extend(frame[3*(row + left):3*(row + right)])
        timeline.append(poses[frame])
    out.write('struct EyePatch { int x, y, w, h, offset; };\n')
    out.write('inline constexpr EyePatch eyePatches[] = {\n')
    for patch in patches:
        out.write('{' + ','.join(map(str, patch)) + '},\n')
    out.write('};\ninline constexpr uint16_t eyeTimeline[] = {' + ','.join(map(str, timeline)) + '};\n')
    write_bytes(out, 'eyePixels', packed)


def main():
    path = Path(sys.argv[1])
    with path.open('w') as out:
        out.write('// Generated by tools/liquid_art.py\n#pragma once\n#include <cstdint>\nnamespace necronomicon::art {\n')
        for name, filename, magic, w, h, channels in [('titleMask', 'title-mask.pgm', 'P5', 247 * TITLE_SCALE, 36, 1),
                                                       ('thorn', 'silver-thorn.ppm', 'P6', 320, 138, 3),
                                                       ('eye', 'slipstream-eye.ppm', 'P6', EYE_WIDTH, EYE_HEIGHT * EYE_FRAMES, 3),
                                                       ('controls', 'controls.ppm', 'P6', 320, 238, 3)]:
            data = (ROOT / 'assets' / 'liquid-light' / filename).read_bytes()
            header = f'{magic}\n{w} {h}\n255\n'.encode()
            if not data.startswith(header) or len(data) != len(header) + channels * w * h:
                raise ValueError(f'Unexpected print dimensions for {name}')
            pixels = data[len(header):]
            if name == 'titleMask':
                write_silver_title(out, pixels, w, h)
                continue
            if name == 'eye':
                write_menu_eye(out, pixels)
                continue
            if name == 'thorn':
                row = 3 * w
                write_bytes(out, 'rhythmDeck', pixels[:110*row])
                write_bytes(out, 'focusPlate', pixels[110*row:124*row])
                write_bytes(out, 'editPlate', pixels[124*row:])
                continue
            if name != 'controls':
                write_bytes(out, name, pixels)
                continue
            # Only the used rectangles of the printed control sheet ship on the MPC.
            for tile, x, y, width, height in (
                ('knobFrame', 0, 0, 106, 42), ('seqFrame', 0, 42, 159, 28),
                ('readout', 212, 0, 73, 36),
                ('settings', 0, 70, 320, 110), ('cursor', 0, 180, 312, 14),
                ('help', 0, 194, 320, 42), ('seam', 0, 236, 320, 2),
            ):
                rows = (3 * (row * w + x) for row in range(y, y + height))
                write_bytes(out, tile, b''.join(pixels[i:i + 3 * width] for i in rows))
        out.write('inline constexpr uint32_t stainedPalette[] = {' + ','.join(map(hex, palette(STOPS))) + '};\n')
        out.write('inline constexpr uint32_t driftPalette[][256] = {\n')
        for key in drift_keys():
            out.write('{' + ','.join(map(hex, key)) + '},\n')
        out.write('};\n')
        write_bytes(out, 'lens', b''.join(lenses()))
        out.write('}\n')


if __name__ == '__main__':
    main()
