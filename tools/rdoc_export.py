import os
import re
import sys
import json
import struct
import argparse

import renderdoc as rd
# Use this script in RenderDoc's "Python Scripting"

UI_OUT = "rdoc_export"
UI_EID = "" # example: "1,2,3" or "draws" or "dispatches" or "all" or "1-100"
UI_TEX = "png"

RNAMES = {}

VFIELD = {
    "Float": "f32v",
    "Half": "f32v",
    "Double": "f64v",
    "SInt": "s32v",
    "UInt": "u32v",
    "Bool": "u32v",
    "Enum": "u32v",
    "SShort": "s16v",
    "UShort": "u16v",
    "SLong": "s64v",
    "ULong": "u64v",
    "GPUPointer": "u64v",
    "SByte": "s8v",
    "UByte": "u8v",
}


def ename(e):
    n = getattr(e, "name", None)
    if n:
        return str(n)
    return str(e)


def safe(fn, *a):
    try:
        return fn(*a)
    except Exception:
        return None


def guard(errs, name, fn, *a):
    try:
        return fn(*a)
    except Exception as e:
        errs.append("%s: %r" % (name, e))
        return None


def clean(s):
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", str(s))[:80]


def is_null(r):
    return r == rd.ResourceId.Null()


def is_ok(r):
    if hasattr(r, "OK"):
        return r.OK()
    return ename(r) == "Succeeded"


def ser(o, ids=None, d=0):
    if o is None or isinstance(o, (bool, float, str)):
        return o
    if isinstance(o, int):
        if hasattr(o, "name"):
            return ename(o)
        return o
    if isinstance(o, rd.ResourceId):
        if is_null(o):
            return None
        k = str(o)
        if ids is not None:
            ids[k] = o
        return {"id": k, "name": RNAMES.get(k)}
    if isinstance(o, (bytes, bytearray)):
        return {"bytes": len(o)}
    if d > 10:
        return str(o)
    if isinstance(o, dict):
        return {str(k): ser(v, ids, d + 1) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [ser(x, ids, d + 1) for x in o]
    out = {}
    for k in dir(o):
        if k.startswith("_") or k in ("thisown", "this"):
            continue
        try:
            v = getattr(o, k)
        except Exception:
            continue
        if callable(v):
            continue
        out[k] = ser(v, ids, d + 1)
    return out


def wjson(path, obj, ids=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(ser(obj, ids), f, indent=1, default=str)


def wtext(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def wbin(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(bytes(data))


def flagnames(cls, v):
    r = []
    iv = int(v)
    for n in dir(cls):
        if n.startswith("_"):
            continue
        m = getattr(cls, n, None)
        if isinstance(m, int) and not isinstance(m, bool) and int(m) != 0 and (iv & int(m)) == int(m):
            r.append(n)
    return r


def sdj(o, d=0):
    n = o.NumChildren() if hasattr(o, "NumChildren") else 0
    if n == 0 or d > 6:
        return {"name": o.name, "value": o.AsString()}
    return {"name": o.name, "children": [sdj(o.GetChild(i), d + 1) for i in range(n)]}


def flatten(c):
    res = []

    def rec(acts, parent, depth):
        for a in acts:
            res.append((a, parent, depth))
            rec(a.children, a, depth + 1)

    rec(c.GetRootActions(), None, 0)
    return res


def stage_list():
    out = []
    seen = set()
    for n in ("Vertex", "Hull", "Domain", "Geometry", "Pixel", "Compute", "Task", "Mesh"):
        s = getattr(rd.ShaderStage, n, None)
        if s is None or int(s) in seen:
            continue
        seen.add(int(s))
        out.append((n, s))
    return out


def pipe_obj(state, stage):
    if int(stage) == int(rd.ShaderStage.Compute):
        return safe(state.GetComputePipelineObject)
    return safe(state.GetGraphicsPipelineObject)


def var_val(v):
    f = VFIELD.get(ename(v.type))
    if f is None:
        return None
    n = max(int(v.rows), 1) * max(int(v.columns), 1)
    arr = safe(lambda: list(getattr(v.value, f)))
    if arr is None:
        return None
    return arr[:n]


def var_json(v):
    d = {"name": v.name, "type": ename(v.type), "rows": int(v.rows), "columns": int(v.columns)}
    if len(v.members) > 0:
        d["members"] = [var_json(m) for m in v.members]
    else:
        d["value"] = var_val(v)
    return d


def read_cb(c, pipe, shader, stage, entry, i, rid, off, size):
    variants = (
        (pipe, shader, stage, entry, i, rid, off, size),
        (pipe, shader, entry, i, rid, off, size),
    )
    for args in variants:
        try:
            return c.GetCBufferVariableContents(*args)
        except TypeError:
            continue
    return None


def comp_code(fmt):
    w = int(fmt.compByteWidth)
    t = ename(fmt.compType)
    if t == "Float":
        return {2: "e", 4: "f", 8: "d"}.get(w)
    if t in ("SInt", "SNorm", "SScaled"):
        return {1: "b", 2: "h", 4: "i", 8: "q"}.get(w)
    return {1: "B", 2: "H", 4: "I", 8: "Q"}.get(w)


def decode(fmt, buf, off):
    if ename(fmt.type) != "Regular":
        return None
    code = comp_code(fmt)
    if code is None:
        return None
    w = int(fmt.compByteWidth)
    n = int(fmt.compCount)
    if off < 0 or off + w * n > len(buf):
        return None
    vals = list(struct.unpack_from("<" + code * n, buf, off))
    t = ename(fmt.compType)
    if t in ("UNorm", "UNormSRGB"):
        m = float((1 << (8 * w)) - 1)
        vals = [v / m for v in vals]
    elif t == "SNorm":
        m = float((1 << (8 * w - 1)) - 1)
        vals = [max(v / m, -1.0) for v in vals]
    return vals


def read_idx(c, rid, off, stride, n):
    data = safe(c.GetBufferData, rid, off, n * stride)
    if not data:
        return None
    code = {1: "B", 2: "H", 4: "I"}[stride]
    cnt = len(data) // stride
    return list(struct.unpack_from("<%d%s" % (cnt, code), data, 0))


def read_indices(c, act, ib, n):
    indexed = int(act.flags) & int(rd.ActionFlags.Indexed)
    if not indexed or ib is None or is_null(ib.resourceId):
        return list(range(n))
    stride = int(ib.byteStride)
    if stride not in (1, 2, 4):
        stride = 2
    off = int(ib.byteOffset) + int(act.indexOffset) * stride
    return read_idx(c, ib.resourceId, off, stride, n)


def tri_faces(idx, topo):
    f = []
    if topo == "TriangleList":
        for i in range(0, len(idx) - 2, 3):
            f.append((idx[i], idx[i + 1], idx[i + 2]))
    elif topo == "TriangleStrip":
        for i in range(len(idx) - 2):
            a, b, d = idx[i], idx[i + 1], idx[i + 2]
            if a == b or b == d or a == d:
                continue
            f.append((a, b, d) if i % 2 == 0 else (a, d, b))
    return f


def obj_text(pos, idx, topo):
    lines = []
    for p in pos:
        q = list(p or [0.0, 0.0, 0.0]) + [0.0, 0.0, 0.0]
        lines.append("v %r %r %r" % (q[0], q[1], q[2]))
    n = len(pos)
    for f in tri_faces(idx, topo):
        if min(f) >= 0 and max(f) < n:
            lines.append("f %d %d %d" % (f[0] + 1, f[1] + 1, f[2] + 1))
    return "\n".join(lines)


def pos_attr(attrs):
    for i, a in enumerate(attrs):
        if "pos" in a.name.lower() and int(a.format.compCount) >= 3:
            return i
    for i, a in enumerate(attrs):
        if int(a.format.compCount) >= 3 and ename(a.format.compType) == "Float":
            return i
    return None


def write_action(ctx, act, folder, ids):
    sd = ctx["sd"]
    d = {
        "eventId": act.eventId,
        "actionId": act.actionId,
        "name": act.GetName(sd),
        "flags": flagnames(rd.ActionFlags, act.flags),
    }
    for k in (
        "customName",
        "numIndices",
        "numInstances",
        "baseVertex",
        "vertexOffset",
        "instanceOffset",
        "indexOffset",
        "drawIndex",
        "dispatchDimension",
        "dispatchThreadsDimension",
        "dispatchBase",
    ):
        if hasattr(act, k):
            d[k] = getattr(act, k)
    d["outputs"] = list(act.outputs)
    d["depthOut"] = act.depthOut
    chain = []
    p = ctx["parent"].get(act.eventId)
    while p is not None:
        chain.append(p.GetName(sd))
        p = ctx["parent"].get(p.eventId)
    d["parents"] = list(reversed(chain))
    d["events"] = [{"eventId": e.eventId, "chunkIndex": e.chunkIndex} for e in act.events]
    wjson(folder + "/get_action.json", d, ids)


def write_api_calls(ctx, act, folder):
    sd = ctx["sd"]
    out = []
    for e in act.events:
        out.append({"eventId": e.eventId, "chunk": sdj(sd.chunks[e.chunkIndex])})
    wjson(folder + "/api_calls.json", out)


def export_pipeline(c, state, folder, ids):
    api = ename(c.GetAPIProperties().pipelineType)
    getter = {
        "D3D11": "GetD3D11PipelineState",
        "D3D12": "GetD3D12PipelineState",
        "OpenGL": "GetGLPipelineState",
        "Vulkan": "GetVulkanPipelineState",
    }.get(api)
    raw = None
    if getter and hasattr(c, getter):
        raw = safe(getattr(c, getter))
    shaders = {}
    for sname, stage in stage_list():
        sh = safe(state.GetShader, stage)
        if sh is not None and not is_null(sh):
            shaders[sname] = {"shader": sh, "entryPoint": safe(state.GetShaderEntryPoint, stage)}
    summary = {
        "api": api,
        "topology": safe(state.GetPrimitiveTopology),
        "viewport0": safe(state.GetViewport, 0),
        "scissor0": safe(state.GetScissor, 0),
        "colorBlends": safe(state.GetColorBlends),
        "stencilFaces": safe(state.GetStencilFaces),
        "shaders": shaders,
        "outputTargets": safe(state.GetOutputTargets),
        "depthTarget": safe(state.GetDepthTarget),
    }
    wjson(folder + "/get_pipeline_state.json", {"summary": summary, "raw": raw}, ids)


def cb_binding(state, stage, i):
    rid = rd.ResourceId.Null()
    fn = getattr(state, "GetConstantBlock", None) or getattr(state, "GetConstantBuffer", None)
    if fn is None:
        return rid, 0, 0
    b = safe(fn, stage, i, 0)
    if b is None:
        return rid, 0, 0
    d = getattr(b, "descriptor", b)
    r = getattr(d, "resource", None)
    if r is None:
        r = getattr(d, "resourceId", None)
    if r is not None:
        rid = r
    return rid, int(getattr(d, "byteOffset", 0)), int(getattr(d, "byteSize", 0))


def export_stage(c, state, sname, stage, folder, ids):
    refl = state.GetShaderReflection(stage)
    shader = state.GetShader(stage)
    entry = safe(state.GetShaderEntryPoint, stage)
    pipe = pipe_obj(state, stage)
    if pipe is None:
        pipe = rd.ResourceId.Null()
    wjson(
        "%s/get_shader_reflection_%s.json" % (folder, sname),
        {"shader": shader, "entryPoint": entry, "reflection": refl},
        ids,
    )
    raw = safe(lambda: bytes(refl.rawBytes))
    if raw:
        wbin("%s/shader_%s.bin" % (folder, sname), raw)
    for t in safe(c.GetDisassemblyTargets, True) or []:
        txt = safe(c.DisassembleShader, pipe, refl, t)
        if txt:
            wtext("%s/disassemble_shader_%s_%s.txt" % (folder, sname, clean(t)), txt)
    files = safe(lambda: list(refl.debugInfo.files)) or []
    for f in files:
        fn = clean(os.path.basename(str(f.filename).replace("\\", "/"))) or "source"
        wtext("%s/shader_%s_src_%s" % (folder, sname, fn), f.contents)
    blocks = []
    for i, cb in enumerate(refl.constantBlocks):
        rid, off, size = cb_binding(state, stage, i)
        vs = read_cb(c, pipe, shader, stage, entry, i, rid, off, size)
        blocks.append(
            {
                "index": i,
                "name": cb.name,
                "bindPoint": getattr(cb, "fixedBindNumber", getattr(cb, "bindPoint", None)),
                "buffer": rid,
                "byteOffset": off,
                "byteSize": size,
                "variables": [var_json(v) for v in vs] if vs is not None else None,
            }
        )
    wjson("%s/get_cbuffer_contents_%s.json" % (folder, sname), blocks, ids)


def export_bindings(state, sname, stage, folder, ids):
    d = {
        "readOnly": safe(state.GetReadOnlyResources, stage),
        "readWrite": safe(state.GetReadWriteResources, stage),
        "samplers": safe(state.GetSamplers, stage),
    }
    wjson("%s/get_shader_bindings_%s.json" % (folder, sname), d, ids)


def export_mesh(c, state, act, folder, cfg, ids):
    attrs = safe(state.GetVertexInputs) or []
    vbs = safe(state.GetVBuffers) or []
    ib = safe(state.GetIBuffer)
    top = ename(safe(state.GetPrimitiveTopology))
    wjson(
        folder + "/get_vertex_inputs.json",
        {"attributes": attrs, "vertexBuffers": vbs, "indexBuffer": ib, "topology": top},
        ids,
    )
    n = int(act.numIndices)
    if n <= 0 or n > cfg.maxverts or not attrs:
        return
    idx = read_indices(c, act, ib, n)
    if not idx:
        return
    count = max(idx) + 1
    if count > cfg.maxverts:
        return
    base = int(act.baseVertex)
    cache = {}
    table = []
    for v in range(count):
        row = []
        for a in attrs:
            vals = None
            k = int(a.vertexBuffer)
            if 0 <= k < len(vbs):
                vb = vbs[k]
                if k not in cache:
                    cache[k] = safe(c.GetBufferData, vb.resourceId, 0, 0) or b""
                i = v + base
                if a.perInstance:
                    rate = max(int(a.instanceRate), 1)
                    i = int(act.instanceOffset) // rate
                off = int(vb.byteOffset) + int(vb.byteStride) * i + int(a.byteOffset)
                vals = decode(a.format, cache[k], off)
            row.append(vals)
        table.append(row)
    header = ["vertexIndex"]
    for a in attrs:
        for j in range(int(a.format.compCount)):
            header.append("%s.%d" % (a.name, j))
    lines = [",".join(header)]
    for v, row in enumerate(table):
        cells = [str(v)]
        for a, vals in zip(attrs, row):
            k = int(a.format.compCount)
            if vals is None:
                cells.extend([""] * k)
            else:
                cells.extend(repr(x) for x in vals)
        lines.append(",".join(cells))
    wtext(folder + "/export_mesh_vertices.csv", "\n".join(lines))
    pi = pos_attr(attrs)
    if pi is not None:
        wtext(folder + "/export_mesh.obj", obj_text([r[pi] for r in table], idx, top))


def export_postvs(c, act, folder, cfg, ids):
    for nm in ("VSOut", "GSOut"):
        stage = getattr(rd.MeshDataStage, nm, None)
        if stage is None:
            continue
        pv = safe(c.GetPostVSData, 0, 0, stage)
        if pv is None or is_null(pv.vertexResourceId):
            continue
        wjson("%s/get_post_vs_data_%s.json" % (folder, nm), pv, ids)
        n = int(pv.numIndices)
        if n <= 0 or n > cfg.maxverts:
            continue
        stride = int(pv.indexByteStride)
        if stride in (1, 2, 4) and not is_null(pv.indexResourceId):
            raw = read_idx(c, pv.indexResourceId, int(pv.indexByteOffset), stride, n)
            if not raw:
                continue
            idx = [i + int(pv.baseVertex) for i in raw]
        else:
            idx = list(range(n))
        vdata = safe(c.GetBufferData, pv.vertexResourceId, int(pv.vertexByteOffset), 0)
        if not vdata:
            continue
        count = max(idx) + 1
        if count > cfg.maxverts or min(idx) < 0:
            continue
        vstride = int(pv.vertexByteStride)
        table = [decode(pv.format, vdata, vstride * v) for v in range(count)]
        k = int(pv.format.compCount)
        lines = [",".join(["vertexIndex"] + ["c%d" % j for j in range(k)])]
        for v, vals in enumerate(table):
            cells = [str(v)]
            if vals is None:
                cells.extend([""] * k)
            else:
                cells.extend(repr(x) for x in vals)
            lines.append(",".join(cells))
        wtext("%s/get_post_vs_data_%s.csv" % (folder, nm), "\n".join(lines))
        wtext("%s/export_mesh_%s.obj" % (folder, nm), obj_text(table, idx, ename(pv.topology)))


def gather_roles(state, stages):
    roles = []
    for i, t in enumerate(safe(state.GetOutputTargets) or []):
        roles.append(("rt%d" % i, t))
    dt = safe(state.GetDepthTarget)
    if dt is not None:
        roles.append(("depth", dt))
    for sname, stage in stages:
        for i, r in enumerate(safe(state.GetReadOnlyResources, stage) or []):
            roles.append(("%s_ro%d" % (sname, i), r))
        for i, r in enumerate(safe(state.GetReadWriteResources, stage) or []):
            roles.append(("%s_rw%d" % (sname, i), r))
    return roles


def minmax(c, rid):
    r = safe(c.GetMinMax, rid, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
    if r is None:
        r = safe(c.GetMinMax, rid, 0, 0, 0, rd.CompType.Typeless)
    return r


def export_textures(c, ctx, cfg, roles, folder, ids, save):
    ft = getattr(rd.FileType, cfg.tex.upper(), rd.FileType.PNG)
    tdir = os.path.join(folder, "export_draw_textures")
    index = []
    saved = {}
    for role, obj in roles:
        found = {}
        ser(obj, found)
        for k, rid in found.items():
            t = ctx["tex"].get(k)
            if t is None:
                continue
            ids[k] = rid
            entry = {
                "role": role,
                "id": k,
                "name": RNAMES.get(k),
                "formatName": safe(t.format.Name),
                "texture": t,
                "minmax": minmax(c, rid),
            }
            if save:
                if k not in saved:
                    os.makedirs(tdir, exist_ok=True)
                    stem = "_".join(x for x in (clean(k), clean(RNAMES.get(k) or "")) if x)
                    fn = "tex_%s.%s" % (stem, cfg.tex.lower())
                    ts = rd.TextureSave()
                    ts.resourceId = rid
                    ts.mip = 0
                    ts.slice.sliceIndex = 0
                    ts.alpha = rd.AlphaMapping.Preserve
                    ts.destType = ft
                    safe(setattr, ts, "channelExtract", -1)
                    safe(setattr, ts, "typeCast", rd.CompType.Typeless)
                    safe(c.SaveTexture, ts, os.path.join(tdir, fn))
                    saved[k] = fn
                entry["file"] = saved[k]
            index.append(entry)
    wjson(folder + "/get_texture_stats.json", index)


def export_buffers(c, ctx, cfg, ids, folder):
    index = []
    bdir = os.path.join(folder, "get_buffer_data")
    for k, rid in ids.items():
        b = ctx["buf"].get(k)
        if b is None:
            continue
        n = min(int(b.length), cfg.maxbuf)
        data = safe(c.GetBufferData, rid, 0, n)
        if not data:
            continue
        stem = "_".join(x for x in (clean(k), clean(RNAMES.get(k) or "")) if x)
        fn = "buf_%s.bin" % stem
        wbin(os.path.join(bdir, fn), data)
        index.append({"id": k, "name": RNAMES.get(k), "length": int(b.length), "saved": n, "file": fn})
    wjson(folder + "/get_buffer_data.json", index)


def export_usage(c, ctx, ids, folder):
    out = {}
    for k, rid in ids.items():
        if k in ctx["tex"] or k in ctx["buf"]:
            out[k] = {"name": RNAMES.get(k), "usage": safe(c.GetUsage, rid)}
    wjson(folder + "/get_resource_usage.json", out)


def export_pixel(c, roles, folder, px, py):
    for role, obj in roles:
        if role != "rt0":
            continue
        found = {}
        ser(obj, found)
        for k, rid in found.items():
            h = safe(c.PixelHistory, rid, px, py, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
            if h is None:
                h = safe(c.PixelHistory, rid, px, py, 0, 0, 0, rd.CompType.Typeless)
            wjson("%s/pixel_history_%d_%d.json" % (folder, px, py), {"target": k, "history": h})
            return


def export_event(c, ctx, cfg, eid):
    c.SetFrameEvent(eid, True)
    state = c.GetPipelineState()
    folder = os.path.join(cfg.out, "EID_%06d" % eid)
    os.makedirs(folder, exist_ok=True)
    errs = []
    ids = {}
    act = ctx["ev"].get(eid)
    own = act is not None and act.eventId == eid
    if act is not None:
        guard(errs, "get_action", write_action, ctx, act, folder, ids)
        guard(errs, "api_calls", write_api_calls, ctx, act, folder)
    guard(errs, "get_pipeline_state", export_pipeline, c, state, folder, ids)
    stages = []
    for sname, stage in stage_list():
        if safe(state.GetShaderReflection, stage) is not None:
            stages.append((sname, stage))
    for sname, stage in stages:
        guard(errs, "shader_" + sname, export_stage, c, state, sname, stage, folder, ids)
        guard(errs, "bindings_" + sname, export_bindings, state, sname, stage, folder, ids)
    if own:
        guard(errs, "export_mesh", export_mesh, c, state, act, folder, cfg, ids)
        guard(errs, "post_vs", export_postvs, c, act, folder, cfg, ids)
    roles = gather_roles(state, stages)
    guard(errs, "textures", export_textures, c, ctx, cfg, roles, folder, ids, not cfg.no_tex)
    if cfg.pixel:
        px, py = [int(v) for v in cfg.pixel.split(",")]
        guard(errs, "pixel_history", export_pixel, c, roles, folder, px, py)
    guard(errs, "usage", export_usage, c, ctx, ids, folder)
    if not cfg.no_buf:
        guard(errs, "buffers", export_buffers, c, ctx, cfg, ids, folder)
    if errs:
        wtext(folder + "/errors.txt", "\n".join(errs))
    print("EID %d done, errors: %d" % (eid, len(errs)))


def export_frame(c, ctx, cfg):
    out = cfg.out
    wjson(
        out + "/get_capture_info.json",
        {"capture": cfg.capture, "api": c.GetAPIProperties(), "frame": safe(c.GetFrameInfo)},
    )
    wjson(out + "/list_resources.json", c.GetResources())
    wjson(out + "/list_textures.json", c.GetTextures())
    wjson(out + "/list_buffers.json", c.GetBuffers())
    sd = ctx["sd"]
    acts = []
    counts = {"actions": 0, "draws": 0, "dispatches": 0, "markers": 0}
    for a, p, depth in ctx["flat"]:
        fl = flagnames(rd.ActionFlags, a.flags)
        acts.append(
            {
                "eventId": a.eventId,
                "actionId": a.actionId,
                "name": a.GetName(sd),
                "flags": fl,
                "depth": depth,
                "parent": p.eventId if p is not None else None,
            }
        )
        counts["actions"] += 1
        if "Drawcall" in fl:
            counts["draws"] += 1
        if "Dispatch" in fl:
            counts["dispatches"] += 1
        if "PushMarker" in fl or "SetMarker" in fl:
            counts["markers"] += 1
    wjson(out + "/list_actions.json", acts)
    counts["textures"] = len(ctx["tex"])
    counts["buffers"] = len(ctx["buf"])
    counts["resources"] = len(RNAMES)
    wjson(out + "/get_frame_overview.json", counts)


def parse_eids(spec, flat, ev):
    if isinstance(spec, (list, tuple)):
        spec = ",".join(str(s) for s in spec)
    tokens = [t for t in re.split(r"[,\s;]+", str(spec).strip().lower()) if t]
    dmask = int(rd.ActionFlags.Drawcall)
    xmask = int(rd.ActionFlags.Dispatch)
    out = []
    for t in tokens:
        if t == "all":
            out.extend(a.eventId for a, p, d in flat if int(a.flags) & (dmask | xmask))
        elif t == "draws":
            out.extend(a.eventId for a, p, d in flat if int(a.flags) & dmask)
        elif t == "dispatches":
            out.extend(a.eventId for a, p, d in flat if int(a.flags) & xmask)
        elif "-" in t:
            lo, hi = t.split("-", 1)
            out.extend(range(int(lo), int(hi) + 1))
        else:
            out.append(int(t))
    last = max(ev) if ev else 0
    res = []
    for e in sorted(set(out)):
        if e < 1 or e > last:
            print("EID %d skipped: out of range 1-%d" % (e, last))
            continue
        res.append(e)
    return res


def run(c, cfg):
    RNAMES.clear()
    for r in c.GetResources():
        RNAMES[str(r.resourceId)] = r.name
    flat = flatten(c)
    ctx = {
        "sd": c.GetStructuredFile(),
        "flat": flat,
        "parent": {a.eventId: p for a, p, d in flat},
        "ev": {},
        "tex": {str(t.resourceId): t for t in c.GetTextures()},
        "buf": {str(b.resourceId): b for b in c.GetBuffers()},
    }
    for a, p, d in flat:
        for e in a.events:
            ctx["ev"][e.eventId] = a
    os.makedirs(cfg.out, exist_ok=True)
    export_frame(c, ctx, cfg)
    eids = parse_eids(cfg.eid, flat, ctx["ev"])
    total = len(eids)
    failed = []
    for n, eid in enumerate(eids, 1):
        print("[%d/%d] EID %d" % (n, total, eid))
        try:
            export_event(c, ctx, cfg, eid)
        except Exception as e:
            failed.append(eid)
            print("EID %d failed: %r" % (eid, e))
    wjson(cfg.out + "/export_summary.json", {"requested": eids, "failed": failed})
    print("Done: %d exported, %d failed, %s" % (total - len(failed), len(failed), os.path.abspath(cfg.out)))


def open_capture(path):
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    res = cap.OpenFile(path, "", None)
    if not is_ok(res):
        raise RuntimeError("OpenFile failed: %s" % str(res))
    res, c = cap.OpenCapture(rd.ReplayOptions(), None)
    if not is_ok(res):
        raise RuntimeError("OpenCapture failed: %s" % str(res))
    return cap, c


def main():
    p = argparse.ArgumentParser()
    p.add_argument("capture")
    p.add_argument("--eid", nargs="+", default=["all"])
    p.add_argument("--out", default="rdoc_export")
    p.add_argument("--tex", default="png")
    p.add_argument("--maxbuf", type=int, default=67108864)
    p.add_argument("--maxverts", type=int, default=2000000)
    p.add_argument("--pixel", default="")
    p.add_argument("--no-tex", action="store_true")
    p.add_argument("--no-buf", action="store_true")
    cfg = p.parse_args()
    cap, c = open_capture(cfg.capture)
    try:
        run(c, cfg)
    finally:
        c.Shutdown()
        cap.Shutdown()
        rd.ShutdownReplay()


if "pyrenderdoc" in globals():
    ui_cfg = argparse.Namespace(
        capture=pyrenderdoc.GetCaptureFilename(),
        eid=UI_EID or str(pyrenderdoc.CurEvent()),
        out=UI_OUT,
        tex=UI_TEX,
        maxbuf=67108864,
        maxverts=2000000,
        pixel="",
        no_tex=False,
        no_buf=False,
    )
    pyrenderdoc.Replay().BlockInvoke(lambda ctrl: run(ctrl, ui_cfg))
elif __name__ == "__main__":
    main()