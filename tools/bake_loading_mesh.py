#!/usr/bin/env python3
"""Bake a rigged/animated FBX character into EDPE's compact EDPM binary format."""

import argparse
import math
import struct
import zlib
from pathlib import Path

EDPM_MAGIC = 0x4D504445  # 'EDPM'
EDPM_VERSION = 1
MAX_BONES = 48
KTIME_PER_SEC = 46186158000.0


def read_fbx_array(data: bytes, pos: int, tc: str):
    arr_len, enc, comp_len = struct.unpack_from("<III", data, pos)
    pos += 12
    raw = data[pos : pos + comp_len]
    pos += comp_len
    if enc == 1:
        raw = zlib.decompress(raw)
    fmt_map = {"f": "f", "d": "d", "l": "q", "i": "i", "b": "B"}
    arr = struct.unpack(f"<{arr_len}{fmt_map[tc]}", raw)
    return arr, pos


def read_fbx_node(data: bytes, pos: int, is64: bool):
    if is64:
        end_offset, num_props, prop_list_len = struct.unpack_from("<QQQ", data, pos)
        pos += 24
    else:
        end_offset, num_props, prop_list_len = struct.unpack_from("<III", data, pos)
        pos += 12
    name_len = data[pos]
    pos += 1
    if end_offset == 0 and num_props == 0 and prop_list_len == 0 and name_len == 0:
        return None, pos
    name = data[pos : pos + name_len].decode("utf-8", errors="replace")
    pos += name_len
    props = []
    for _ in range(num_props):
        tc = chr(data[pos])
        pos += 1
        if tc == "Y":
            (val,) = struct.unpack_from("<h", data, pos)
            pos += 2
        elif tc == "C":
            val = data[pos] != 0
            pos += 1
        elif tc == "I":
            (val,) = struct.unpack_from("<i", data, pos)
            pos += 4
        elif tc == "F":
            (val,) = struct.unpack_from("<f", data, pos)
            pos += 4
        elif tc == "D":
            (val,) = struct.unpack_from("<d", data, pos)
            pos += 8
        elif tc == "L":
            (val,) = struct.unpack_from("<q", data, pos)
            pos += 8
        elif tc in ("S", "R"):
            (slen,) = struct.unpack_from("<I", data, pos)
            pos += 4
            raw = data[pos : pos + slen]
            pos += slen
            val = raw.decode("utf-8", errors="replace") if tc == "S" else raw
        elif tc in ("f", "d", "l", "i", "b"):
            val, pos = read_fbx_array(data, pos, tc)
        else:
            raise ValueError(f"Unsupported FBX property type '{tc}' at {pos}")
        props.append(val)
    children = []
    while pos < end_offset:
        child, pos = read_fbx_node(data, pos, is64)
        if child is None:
            break
        children.append(child)
    return (name, props, children), end_offset


def parse_fbx(path: Path):
    data = path.read_bytes()
    if not data.startswith(b"Kaydara FBX Binary  \x00"):
        raise ValueError(f"Not a binary FBX file: {path}")
    (ver,) = struct.unpack_from("<I", data, 23)
    is64 = ver >= 7500
    pos = 27
    nodes = {}
    while pos < len(data):
        n, pos = read_fbx_node(data, pos, is64)
        if n is None:
            break
        nodes[n[0]] = n
    return nodes


def mat_mul(A, B):
    return [[sum(A[i][k] * B[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def mat_from_fbx16(a):
    # FBX stores row-vector matrices; transpose for column-vector M * v.
    return [[a[j * 4 + i] for j in range(4)] for i in range(4)]


def rot_xyz(rx_deg: float, ry_deg: float, rz_deg: float):
    rx, ry, rz = map(math.radians, (rx_deg, ry_deg, rz_deg))
    cx, sx = math.cos(rx), math.sin(rx)
    cy, sy = math.cos(ry), math.sin(ry)
    cz, sz = math.cos(rz), math.sin(rz)
    Rx = [[1.0, 0.0, 0.0, 0.0], [0.0, cx, -sx, 0.0], [0.0, sx, cx, 0.0], [0.0, 0.0, 0.0, 1.0]]
    Ry = [[cy, 0.0, sy, 0.0], [0.0, 1.0, 0.0, 0.0], [-sy, 0.0, cy, 0.0], [0.0, 0.0, 0.0, 1.0]]
    Rz = [[cz, -sz, 0.0, 0.0], [sz, cz, 0.0, 0.0], [0.0, 0.0, 1.0, 0.0], [0.0, 0.0, 0.0, 1.0]]
    return mat_mul(Rz, mat_mul(Ry, Rx))


def trs_mat(t, pre_r, r, s):
    T = [[1.0, 0.0, 0.0, t[0]], [0.0, 1.0, 0.0, t[1]], [0.0, 0.0, 1.0, t[2]], [0.0, 0.0, 0.0, 1.0]]
    S = [[s[0], 0.0, 0.0, 0.0], [0.0, s[1], 0.0, 0.0], [0.0, 0.0, s[2], 0.0], [0.0, 0.0, 0.0, 1.0]]
    return mat_mul(T, mat_mul(rot_xyz(*pre_r), mat_mul(rot_xyz(*r), S)))


def diag_mat(sx: float, sy: float, sz: float, tx: float = 0.0, ty: float = 0.0, tz: float = 0.0):
    return [[sx, 0.0, 0.0, tx], [0.0, sy, 0.0, ty], [0.0, 0.0, sz, tz], [0.0, 0.0, 0.0, 1.0]]


def eval_curve(kt, kv, t: float) -> float:
    if not kt:
        return 0.0
    if t <= kt[0]:
        return kv[0]
    if t >= kt[-1]:
        return kv[-1]
    lo, hi = 0, len(kt) - 1
    while lo + 1 < hi:
        mid = (lo + hi) // 2
        if kt[mid] <= t:
            lo = mid
        else:
            hi = mid
    dt = kt[hi] - kt[lo]
    if dt <= 1e-12:
        return kv[lo]
    a = (t - kt[lo]) / dt
    return kv[lo] * (1.0 - a) + kv[hi] * a


def normalize3(v):
    l = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    if l < 1e-12:
        return (0.0, 1.0, 0.0)
    return (v[0] / l, v[1] / l, v[2] / l)


def dot3(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross3(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def bake(input_fbx: Path, output_bin: Path, num_frames: int = 88):
    nodes = parse_fbx(input_fbx)
    objs = nodes["Objects"][2]
    by_id = {cp[0]: (cn, cp, cc) for cn, cp, cc in objs}

    oo_parent = {}
    oo_children = {}
    op_conns = []
    for _, cp, _ in nodes["Connections"][2]:
        if cp[0] == "OO":
            oo_parent.setdefault(cp[1], []).append(cp[2])
            oo_children.setdefault(cp[2], []).append(cp[1])
        elif cp[0] == "OP":
            op_conns.append((cp[1], cp[2], cp[3]))

    # Pick longest AnimationStack
    stacks = []
    for cn, cp, cc in objs:
        if cn == "AnimationStack":
            stop_ktime = 0
            for sn, sp, sc in cc:
                if sn == "Properties70":
                    for _, pp, _ in sc:
                        if pp[0] in ("LocalStop", "ReferenceStop") and len(pp) >= 5:
                            stop_ktime = max(stop_ktime, int(pp[4]))
            stacks.append((stop_ktime, cp[0], cp[1].split("\x00")[0]))
    if not stacks:
        raise ValueError("No AnimationStack found in FBX")
    stacks.sort(reverse=True)
    best_stop_ktime, stack_id, stack_name = stacks[0]
    duration = best_stop_ktime / KTIME_PER_SEC

    layers = [c for c in oo_children.get(stack_id, []) if c in by_id and by_id[c][0] == "AnimationLayer"]
    if not layers:
        raise ValueError(f"No AnimationLayer under stack {stack_name}")
    layer_cnodes = set(oo_children.get(layers[0], []))

    anim = {}
    max_curve_t = 0.0
    for cnid, target_id, prop_name in op_conns:
        if cnid in layer_cnodes and target_id in by_id and by_id[target_id][0] == "Model":
            axis_curves = {}
            for cid, dest_cnid, axis_prop in op_conns:
                if dest_cnid == cnid and cid in by_id and by_id[cid][0] == "AnimationCurve":
                    kt = kv = None
                    for sn, sp, _ in by_id[cid][2]:
                        if sn == "KeyTime":
                            kt = [x / KTIME_PER_SEC for x in sp[0]]
                        elif sn == "KeyValueFloat":
                            kv = list(sp[0])
                    if kt and kv:
                        axis_curves[axis_prop] = (kt, kv)
                        max_curve_t = max(max_curve_t, kt[-1])
            anim[(target_id, prop_name)] = axis_curves
    if max_curve_t > 0.0:
        duration = max_curve_t

    def get_model_props(mid):
        t = [0.0, 0.0, 0.0]
        r = [0.0, 0.0, 0.0]
        s = [1.0, 1.0, 1.0]
        pre_r = [0.0, 0.0, 0.0]
        for sn, sp, sc in by_id[mid][2]:
            if sn == "Properties70":
                for _, pp, _ in sc:
                    if pp[0] == "Lcl Translation":
                        t = [float(pp[4]), float(pp[5]), float(pp[6])]
                    elif pp[0] == "Lcl Rotation":
                        r = [float(pp[4]), float(pp[5]), float(pp[6])]
                    elif pp[0] == "Lcl Scaling":
                        s = [float(pp[4]), float(pp[5]), float(pp[6])]
                    elif pp[0] == "PreRotation":
                        pre_r = [float(pp[4]), float(pp[5]), float(pp[6])]
        return t, pre_r, r, s

    def get_world_mat(mid, t_sec, cache):
        if mid in cache:
            return cache[mid]
        lt, lpr, lr, ls = get_model_props(mid)
        for pname, target in (("Lcl Translation", lt), ("Lcl Rotation", lr), ("Lcl Scaling", ls)):
            ac = anim.get((mid, pname))
            if ac:
                for idx, ax in enumerate(("d|X", "d|Y", "d|Z")):
                    if ax in ac:
                        target[idx] = eval_curve(ac[ax][0], ac[ax][1], t_sec)
        local = trs_mat(lt, lpr, lr, ls)
        parents = [x for x in oo_parent.get(mid, []) if x in by_id and by_id[x][0] == "Model"]
        world = mat_mul(get_world_mat(parents[0], t_sec, cache), local) if parents else local
        cache[mid] = world
        return world

    # Geometry
    geoms = [(cp, cc) for cn, cp, cc in objs if cn == "Geometry"]
    if len(geoms) != 1:
        raise ValueError(f"Expected 1 Geometry, found {len(geoms)}")
    geom_cc = geoms[0][1]
    raw_verts = raw_pvi = raw_norms = raw_nidx = raw_uvs = raw_uvidx = None
    for sn, sp, sc in geom_cc:
        if sn == "Vertices":
            raw_verts = sp[0]
        elif sn == "PolygonVertexIndex":
            raw_pvi = sp[0]
        elif sn == "LayerElementNormal":
            for xn, xp, _ in sc:
                if xn == "Normals":
                    raw_norms = xp[0]
                elif xn == "NormalsIndex":
                    raw_nidx = xp[0]
        elif sn == "LayerElementUV":
            for xn, xp, _ in sc:
                if xn == "UV":
                    raw_uvs = xp[0]
                elif xn == "UVIndex":
                    raw_uvidx = xp[0]

    if raw_verts is None or raw_pvi is None or raw_norms is None or raw_uvs is None:
        raise ValueError("Missing required Geometry arrays (Vertices/PolygonVertexIndex/Normals/UV)")

    num_raw_verts = len(raw_verts) // 3

    # Clusters (bones)
    clusters = []
    for cn, cp, cc in objs:
        if cn == "Deformer" and cp[2] == "Cluster":
            cid = cp[0]
            bone_ids = [c for c in oo_children.get(cid, []) if c in by_id and by_id[c][0] == "Model"]
            if not bone_ids:
                continue
            idxs = wts = tf = None
            for sn, sp, _ in cc:
                if sn == "Indexes":
                    idxs = sp[0]
                elif sn == "Weights":
                    wts = sp[0]
                elif sn == "Transform":
                    tf = mat_from_fbx16(sp[0])
            if idxs and len(idxs) > 0:
                clusters.append((bone_ids[0], idxs, wts, tf))

    if len(clusters) > MAX_BONES:
        raise ValueError(f"Cluster count {len(clusters)} exceeds MAX_BONES={MAX_BONES}")

    # Collect per-raw-vertex bone influences (top 4, quantized to sum=255)
    raw_influences = [[] for _ in range(num_raw_verts)]
    for b_idx, (_, idxs, wts, _) in enumerate(clusters):
        for vi, w in zip(idxs, wts):
            if w > 0.0:
                raw_influences[vi].append((w, b_idx))

    packed_bones = []
    packed_weights = []
    for vi in range(num_raw_verts):
        infs = sorted(raw_influences[vi], key=lambda x: -x[0])[:4]
        if not infs:
            packed_bones.append((0, 0, 0, 0))
            packed_weights.append((255, 0, 0, 0))
            continue
        total = sum(w for w, _ in infs)
        if total <= 1e-9:
            packed_bones.append((0, 0, 0, 0))
            packed_weights.append((255, 0, 0, 0))
            continue
        b4 = [0, 0, 0, 0]
        w4 = [0, 0, 0, 0]
        for k, (w, b_idx) in enumerate(infs):
            b4[k] = b_idx
            w4[k] = int(round((w / total) * 255.0))
        diff = 255 - sum(w4)
        w4[0] = max(0, min(255, w4[0] + diff))
        # Adjust if clamping occurred
        rem = 255 - sum(w4)
        if rem != 0:
            for k in range(4):
                cand = w4[k] + rem
                if 0 <= cand <= 255:
                    w4[k] = cand
                    break
        assert sum(w4) == 255
        packed_bones.append(tuple(b4))
        packed_weights.append(tuple(w4))

    # Build unique split vertices and flipped-winding triangle indices
    # Handle both IndexToDirect and Direct mapping gracefully
    direct_normals = raw_nidx is None
    direct_uvs = raw_uvidx is None

    poly_tris = []
    cur_poly = []
    pv_offset = 0
    for idx in raw_pvi:
        end_poly = idx < 0
        v_idx = ~idx if end_poly else idx
        n_idx = pv_offset if direct_normals else raw_nidx[pv_offset]
        uv_idx = pv_offset if direct_uvs else raw_uvidx[pv_offset]
        cur_poly.append((v_idx, n_idx, uv_idx))
        pv_offset += 1
        if end_poly:
            if len(cur_poly) < 3:
                raise ValueError("Degenerate polygon with <3 vertices")
            for k in range(1, len(cur_poly) - 1):
                # Flip winding: (0, k, k+1) -> (0, k+1, k) for diag(-1, 1, 1) reflection into CW D3D11
                poly_tris.append((cur_poly[0], cur_poly[k + 1], cur_poly[k]))
            cur_poly = []

    uniq_map = {}
    vertices_pos = []
    vertices_norm = []
    vertices_uv = []
    vertices_raw_vi = []
    indices = []

    for tri in poly_tris:
        for key in tri:
            out_idx = uniq_map.get(key)
            if out_idx is None:
                out_idx = len(vertices_pos)
                uniq_map[key] = out_idx
                vi, ni, uvi = key
                vertices_pos.append((raw_verts[vi * 3], raw_verts[vi * 3 + 1], raw_verts[vi * 3 + 2]))
                vertices_norm.append(normalize3((raw_norms[ni * 3], raw_norms[ni * 3 + 1], raw_norms[ni * 3 + 2])))
                vertices_uv.append((raw_uvs[uvi * 2], 1.0 - raw_uvs[uvi * 2 + 1]))
                vertices_raw_vi.append(vi)
            indices.append(out_idx)

    num_verts = len(vertices_pos)
    num_indices = len(indices)
    if num_verts > 65535:
        raise ValueError(f"Unique vertex count {num_verts} exceeds uint16 limit 65535")

    # Accumulate tangents & bitangents per vertex in FBX geometry space (using flipped V UVs,
    # keeping original FBX triangle orientation (i0, i2, i1) -> undo flip for tangent basis,
    # then negate handedness for diag(1, 1, -1) reflection).
    tan1 = [[0.0, 0.0, 0.0] for _ in range(num_verts)]
    tan2 = [[0.0, 0.0, 0.0] for _ in range(num_verts)]
    for t_i in range(0, num_indices, 3):
        # indices has (0, k+1, k); original FBX CCW order is (i0, i2, i1)
        i0, i1, i2 = indices[t_i], indices[t_i + 2], indices[t_i + 1]
        p0, p1, p2 = vertices_pos[i0], vertices_pos[i1], vertices_pos[i2]
        uv0, uv1, uv2 = vertices_uv[i0], vertices_uv[i1], vertices_uv[i2]

        x1, y1, z1 = p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]
        x2, y2, z2 = p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]
        s1, t1 = uv1[0] - uv0[0], uv1[1] - uv0[1]
        s2, t2 = uv2[0] - uv0[0], uv2[1] - uv0[1]

        det = s1 * t2 - s2 * t1
        if abs(det) > 1e-12:
            r = 1.0 / det
            sdir = ((t2 * x1 - t1 * x2) * r, (t2 * y1 - t1 * y2) * r, (t2 * z1 - t1 * z2) * r)
            tdir = ((s1 * x2 - s2 * x1) * r, (s1 * y2 - s2 * y1) * r, (s1 * z2 - s2 * z1) * r)
            for idx in (i0, i1, i2):
                tan1[idx][0] += sdir[0]
                tan1[idx][1] += sdir[1]
                tan1[idx][2] += sdir[2]
                tan2[idx][0] += tdir[0]
                tan2[idx][1] += tdir[1]
                tan2[idx][2] += tdir[2]

    vertices_tan = []
    for vi in range(num_verts):
        n = vertices_norm[vi]
        t = tan1[vi]
        ndott = dot3(n, t)
        ortho = (t[0] - n[0] * ndott, t[1] - n[1] * ndott, t[2] - n[2] * ndott)
        olen = math.sqrt(dot3(ortho, ortho))
        if olen > 1e-8:
            tn = (ortho[0] / olen, ortho[1] / olen, ortho[2] / olen)
            c = cross3(n, tn)
            # Negate handedness because M_conv has diag(-1, 1, 1) reflection
            w = 1.0 if dot3(c, tan2[vi]) < 0.0 else -1.0
        else:
            ref = (1.0, 0.0, 0.0) if abs(n[0]) < 0.9 else (0.0, 1.0, 0.0)
            tn = normalize3(cross3(ref, n))
            w = -1.0
        vertices_tan.append((tn[0], tn[1], tn[2], w))

    # M_conv = diag(-1, 1, 1) * diag(1/0.8, 1/0.8, 1/0.8) * Translate(0, +0.5974, +1.9191) * RotX(-23 deg) * diag(0.01, 0.01, 0.01)
    # Maps FBX (+X=left, +Y=up, +Z=front, cm) -> EID 120 local (+X=right, +Y=up, +Z=front, unscaled)
    m_conv = mat_mul(
        diag_mat(-1.0, 1.0, 1.0),
        mat_mul(
            diag_mat(1.25, 1.25, 1.25),
            mat_mul(
                diag_mat(1.0, 1.0, 1.0, 0.0, 0.5974, 1.9191),
                mat_mul(rot_xyz(-23.0, 0.0, 0.0), diag_mat(0.01, 0.01, 0.01)),
            ),
        ),
    )

    bone_frames = []
    min_pt = [1e9, 1e9, 1e9]
    max_pt = [-1e9, -1e9, -1e9]
    for f_idx in range(num_frames):
        t_sec = (f_idx / float(num_frames - 1)) * duration if num_frames > 1 else 0.0
        cache = {}
        frame_mats = []
        for bid, _, _, tf in clusters:
            w_bone = get_world_mat(bid, t_sec, cache)
            m_final = mat_mul(m_conv, mat_mul(w_bone, tf))
            frame_mats.append(m_final)
            for r in range(3):
                bone_frames.extend(m_final[r])
        if f_idx == 0:
            # Verify frame 0 skinned bounds in EID 120 local space
            for vi in range(num_verts):
                px, py, pz = vertices_pos[vi]
                b4 = packed_bones[vertices_raw_vi[vi]]
                w4 = packed_weights[vertices_raw_vi[vi]]
                lx = ly = lz = 0.0
                for k in range(4):
                    if w4[k] == 0:
                        continue
                    fw = w4[k] / 255.0
                    m = frame_mats[b4[k]]
                    lx += fw * (m[0][0] * px + m[0][1] * py + m[0][2] * pz + m[0][3])
                    ly += fw * (m[1][0] * px + m[1][1] * py + m[1][2] * pz + m[1][3])
                    lz += fw * (m[2][0] * px + m[2][1] * py + m[2][2] * pz + m[2][3])
                min_pt[0] = min(min_pt[0], lx)
                min_pt[1] = min(min_pt[1], ly)
                min_pt[2] = min(min_pt[2], lz)
                max_pt[0] = max(max_pt[0], lx)
                max_pt[1] = max(max_pt[1], ly)
                max_pt[2] = max(max_pt[2], lz)

    # Pack binary EDPM
    output_bin.parent.mkdir(parents=True, exist_ok=True)
    with output_bin.open("wb") as out:
        out.write(
            struct.pack(
                "<IIIIIIfI",
                EDPM_MAGIC,
                EDPM_VERSION,
                num_verts,
                num_indices,
                len(clusters),
                num_frames,
                duration,
                0,
            )
        )
        vert_fmt = struct.Struct("<3f3f4f2f4B4B")
        assert vert_fmt.size == 56
        for vi in range(num_verts):
            px, py, pz = vertices_pos[vi]
            nx, ny, nz = vertices_norm[vi]
            tx, ty, tz, tw = vertices_tan[vi]
            u, v = vertices_uv[vi]
            b4 = packed_bones[vertices_raw_vi[vi]]
            w4 = packed_weights[vertices_raw_vi[vi]]
            out.write(vert_fmt.pack(px, py, pz, nx, ny, nz, tx, ty, tz, tw, u, v, *b4, *w4))
        out.write(struct.pack(f"<{num_indices}H", *indices))
        if (num_indices % 2) != 0:
            out.write(b"\x00\x00")
        out.write(struct.pack(f"<{len(bone_frames)}f", *bone_frames))

    print(
        f"Baked '{stack_name}' -> {output_bin} ({output_bin.stat().st_size} bytes)\n"
        f"  verts={num_verts}, indices={num_indices} (tris={num_indices // 3}), "
        f"bones={len(clusters)}, frames={num_frames}, duration={duration:.4f}s\n"
        f"  frame0 local bounds: X=[{min_pt[0]:.3f}, {max_pt[0]:.3f}], "
        f"Y=[{min_pt[1]:.3f}, {max_pt[1]:.3f}], Z=[{min_pt[2]:.3f}, {max_pt[2]:.3f}]"
    )


def main():
    parser = argparse.ArgumentParser(description="Bake FBX character animation to EDPM binary.")
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--frames", type=int, default=88)
    args = parser.parse_args()
    bake(args.input, args.output, args.frames)


if __name__ == "__main__":
    main()
