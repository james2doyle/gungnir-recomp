#!/usr/bin/env python3
"""Decode a guest-frame dump and report what interpolation would do with it.

The runtime writes the dump with `--es dump_draws <path> --el dump_after_ms <ms>`
(`dump_guest_frame` in android/app/src/main/cpp/main.cpp): a bounded run of real
guest frames, each draw verbatim as key, through flag, vertex count and every
vertex's x, y, z and texture coordinates.

The point is to answer presentation questions with the frames the game actually
produced instead of inferring them from a recording: whether a bucket's arrival
order is stable, how fast the frame is really moving, and whether a pair's
vertices can be moved independently without the shape collapsing.

Format (little endian, no header):
    repeat frames:  u32 draw_count
                    repeat draws: u64 key, u8 through, u32 vertex_count,
                                  vertex_count * (f32 x, y, z, u, v)
"""
import argparse
import struct
import sys

MAX_DRAWS_PER_FRAME = 100_000  # a corrupt count must not allocate the world

# Mirrors runtime/ge/frame_interpolation.hpp. Kept in sync by
# tests/test_interpolation_report.py, which pins the call sites.
MAX_BUCKET_SCAN = 64
CUT_SCREEN_PIXELS = 0.2 * 480.0   # the original still-scene tolerance, never lowered
FRAME_CUT_PIXELS = 0.5 * 480.0
MOTION_SLACK = 12.0


def load(path):
    with open(path, "rb") as handle:
        data = handle.read()
    frames = []
    offset = 0
    while offset + 4 <= len(data):
        (count,) = struct.unpack_from("<I", data, offset)
        offset += 4
        if count == 0 or count > MAX_DRAWS_PER_FRAME:
            break
        draws = []
        for _ in range(count):
            key, through, vertices = struct.unpack_from("<QBI", data, offset)
            offset += 13
            raw = struct.unpack_from("<" + "f" * (vertices * 5), data, offset)
            offset += vertices * 20
            points = [tuple(raw[i * 5:i * 5 + 5]) for i in range(vertices)]
            draws.append({"key": key, "through": bool(through), "vertices": points})
        frames.append(draws)
    return frames


def centroid(draw):
    points = draw["vertices"]
    return (sum(p[0] for p in points) / len(points),
            sum(p[1] for p in points) / len(points))


def buckets(frame):
    grouped = {}
    for index, draw in enumerate(frame):
        if len(draw["vertices"]) < 3:
            continue
        grouped.setdefault(draw["key"], []).append(index)
    return grouped


def pair_frame(previous, current):
    """Pair like the runtime: small buckets by nearest position, large by arrival."""
    pairs = []
    before, now = buckets(previous), buckets(current)
    for key, indices in now.items():
        earlier = before.get(key)
        if earlier is None:
            continue
        if len(indices) > MAX_BUCKET_SCAN or len(earlier) > MAX_BUCKET_SCAN:
            pairs += [(index, earlier[order]) for order, index in enumerate(indices[:len(earlier)])]
            continue
        candidates = []
        for index in indices:
            cx, cy = centroid(current[index])
            for slot, other in enumerate(earlier):
                px, py = centroid(previous[other])
                candidates.append((abs(cx - px) + abs(cy - py), index, slot))
        candidates.sort()
        used_now, used_before = set(), set()
        for _, index, slot in candidates:
            if index in used_now or slot in used_before:
                continue
            used_now.add(index)
            used_before.add(slot)
            pairs.append((index, earlier[slot]))
    return pairs


def motion_of(previous_draw, current_draw):
    deltas = [(b[0] - a[0], b[1] - a[1])
              for a, b in zip(previous_draw["vertices"], current_draw["vertices"])]
    mean_x = sum(d[0] for d in deltas) / len(deltas)
    mean_y = sum(d[1] for d in deltas) / len(deltas)
    magnitude = sum(abs(d[0]) + abs(d[1]) for d in deltas) / len(deltas)
    deviation = max(abs(d[0] - mean_x) + abs(d[1] - mean_y) for d in deltas)
    return magnitude, deviation, (mean_x, mean_y), deltas


def triangle_area(points, triangle):
    (ax, ay), (bx, by), (cx, cy) = (points[i][:2] for i in triangle)
    return 0.5 * ((bx - ax) * (cy - ay) - (cx - ax) * (by - ay))


def triangles(vertices):
    if vertices < 3:
        return []
    if vertices >= 6:
        return [(0, 1, 2), (3, 4, 5)]
    return [(0, 1, 2)]


def median(values):
    ordered = sorted(values)
    return ordered[len(ordered) // 2] if ordered else 0.0


def percentile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        return 0.0
    return ordered[min(len(ordered) - 1, int(fraction * (len(ordered) - 1) + 0.5))]


def analyse_pair(previous, current, rigid_rule=True):
    """What the runtime's pass would do with this pair of frames.

    `rigid_rule=False` disables the shape-change answer, which is the diagnostic
    mode: it shows the collapses the runtime is preventing.
    """
    pairs = pair_frame(previous, current)
    motions = []
    per_bucket = {}
    for index, other in pairs:
        magnitude, deviation, _, _ = motion_of(previous[other], current[index])
        motions.append(magnitude)
        per_bucket.setdefault(current[index]["key"], []).append(magnitude)
    frame_motion = median(motions)
    collapses = {"vertex": 0, "rigid": 0}
    translated = 0
    for index, other in pairs:
        magnitude, deviation, mean, deltas = motion_of(previous[other], current[index])
        frame_limit = frame_motion * MOTION_SLACK + CUT_SCREEN_PIXELS
        if frame_motion > FRAME_CUT_PIXELS or magnitude > frame_limit:
            continue  # the runtime rejects this pair; nothing is blended
        current_points = current[index]["vertices"]
        previous_points = previous[other]["vertices"]
        vertex_blend = [(0.5 * (b[0] + a[0]), 0.5 * (b[1] + a[1])) + tuple(b[2:])
                        for a, b in zip(previous_points, current_points)]
        shape = triangles(len(current_points))
        would_collapse = False
        for triangle in shape:
            if max(triangle) >= len(current_points):
                continue
            before = triangle_area(previous_points, triangle)
            after = triangle_area(current_points, triangle)
            smallest = min(abs(before), abs(after))
            if smallest < 0.5:
                continue
            middle = triangle_area(vertex_blend, triangle)
            if abs(middle) < 0.5 * smallest or (middle >= 0) != (before >= 0):
                would_collapse = True
                break
        # Only a real collapse justifies the rigid path: perspective moves a
        # mesh's corners by different amounts without changing its shape.
        translate = rigid_rule and would_collapse
        if translate:
            translated += 1
            blend = [(b[0] - 0.5 * mean[0], b[1] - 0.5 * mean[1]) + tuple(b[2:])
                     for b in current_points]
        else:
            blend = vertex_blend
        for triangle in shape:
            if max(triangle) >= len(current_points):
                continue
            before = triangle_area(previous_points, triangle)
            after = triangle_area(current_points, triangle)
            smallest = min(abs(before), abs(after))
            if smallest < 0.5:
                continue
            middle = triangle_area(blend, triangle)
            # A translated pair is presented as this frame's own shape, so its
            # triangles cannot collapse; only a winding mismatch against the
            # previous frame shows up, and that is not a visual defect.
            if translate:
                if (after >= 0) != (before >= 0):
                    collapses["rigid"] += 0
                continue
            if abs(middle) < 0.5 * smallest or (middle >= 0) != (before >= 0):
                collapses["vertex"] += 1
    breaks = 0
    for key, indices in buckets(current).items():
        earlier = buckets(previous).get(key)
        if earlier is None or len(earlier) != len(indices):
            continue
        deltas = [motion_of(previous[earlier[order]], current[index])[0]
                  for order, index in enumerate(indices)]
        limit = median(deltas) * 4.0 + 8.0
        breaks += sum(1 for value in deltas if value > max(limit, 8.0))
    return {
        "pairs": len(pairs),
        "frame_motion": frame_motion,
        "p90": percentile(motions, 0.9),
        "max": max(motions) if motions else 0.0,
        "order_breaks": breaks,
        "collapses_vertex": collapses["vertex"],
        "collapses_rigid": collapses["rigid"],
        "rigid_pairs": translated,
    }


def bucket_breaks(previous, current):
    """Buckets whose arrival order changed, largest displacement first."""
    rows = []
    before = buckets(previous)
    for key, indices in buckets(current).items():
        earlier = before.get(key)
        if earlier is None or len(earlier) != len(indices):
            continue
        deltas = [motion_of(previous[earlier[order]], current[index])[0]
                  for order, index in enumerate(indices)]
        limit = median(deltas) * 4.0 + 8.0
        broken = [d for d in deltas if d > max(limit, 8.0)]
        if not broken:
            continue
        first = current[indices[0]]["vertices"]
        width = max(p[0] for p in first) - min(p[0] for p in first)
        height = max(p[1] for p in first) - min(p[1] for p in first)
        rows.append((max(broken), key, len(indices), len(broken), width, height))
    rows.sort(reverse=True)
    return rows


def report(frames, out=sys.stdout, verbose=False, rigid_rule=True):
    if not frames:
        print("no frames", file=out)
        return
    print(f"frames={len(frames)} draws_per_frame={[len(f) for f in frames]}", file=out)
    first = frames[0]
    print(f"  transform draws={sum(1 for d in first if not d['through'])} "
          f"screen-space draws={sum(1 for d in first if d['through'])}", file=out)
    grouped = buckets(first)
    sizes = sorted((len(v) for v in grouped.values()), reverse=True)
    oversize = [s for s in sizes if s > MAX_BUCKET_SCAN]
    print(f"  identity buckets={len(grouped)} largest={sizes[0] if sizes else 0} "
          f"buckets>{MAX_BUCKET_SCAN}={len(oversize)} draws_in_them={sum(oversize)}", file=out)
    for index in range(len(frames) - 1):
        stats = analyse_pair(frames[index], frames[index + 1], rigid_rule)
        print(f"  {index}->{index + 1}: pairs={stats['pairs']} motion={stats['frame_motion']:.1f} "
              f"p90={stats['p90']:.1f} max={stats['max']:.1f} "
              f"order_breaks={stats['order_breaks']} rigid_pairs={stats['rigid_pairs']} "
              f"collapses vertex={stats['collapses_vertex']} rigid={stats['collapses_rigid']}",
              file=out)
        if verbose:
            for magnitude, key, size, broken, width, height in bucket_breaks(frames[index],
                                                                            frames[index + 1])[:6]:
                print(f"      order break key={key:#x} bucket={size} breaks={broken} "
                      f"worst={magnitude:.1f}px first_draw={width:.0f}x{height:.0f}", file=out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("dump", help="file written by --es dump_draws")
    parser.add_argument("--verbose", action="store_true", help="name the buckets that broke")
    parser.add_argument("--vertex-only", action="store_true",
                        help="disable the shape-change answer, to show what it prevents")
    args = parser.parse_args(argv)
    frames = load(args.dump)
    report(frames, verbose=args.verbose, rigid_rule=not args.vertex_only)
    return 0


if __name__ == "__main__":
    sys.exit(main())
