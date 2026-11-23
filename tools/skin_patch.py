"""Finds the skinned position in the game's skinning vertex shaders (DXIL as text from `dxc -dumpbin`) and patches
code in after it.

The game skins with CDPR's ApplySkinning (REDkit bin\\shaders\\include\\vertexFactory.fx):

    skinnedPosition += mul(localPosition, bone(i)) * (BlendWeights.i / weightSum)   for i = x, y, z, w

In the IR each bone's weight is an `fdiv` (weight / sum of the weights); each bone adds `fmul(that fdiv, value)` per
component, and `fadd`s sum them up (5.01 shaders start the sum with `fadd %t, 0.0`, fast-math ones with the first
product, in either operand order). The skinned component is the topmost `fadd` whose operands reach four different
weight fdivs. Four of them in a row are x, y, z, w of one position; G-buffer shaders skin twice (this frame and the
previous one, for the motion vectors): two positions.

    python tools/skin_patch.py find <file.ll | folder>
    python tools/skin_patch.py patch <in.ll> <out.ll> <inject>     inject: scale:<f> (x, y, z times f, the test)

A patch adds named values (%tw3se_*) right after each component's definition and makes every later use of the
component use the new value. tools/dxil_asm turns the result into a signed shader.
"""
import os
import re
import struct
import sys

DEF = re.compile(r'^\s*(%[\w.]+) = (.*)$')
OPND = re.compile(r'%[\w.]+')


def parse(text):
    """The instructions of the entry function as [(line index, name or None, rhs)], and the lines."""
    lines = text.splitlines()
    body, inside = [], False
    for i, line in enumerate(lines):
        if line.startswith('define void @'):
            inside = True
            continue
        if inside and line.startswith('}'):
            break
        if inside:
            code = line.split(';', 1)[0].rstrip()
            m = DEF.match(code)
            body.append((i, m.group(1) if m else None, m.group(2) if m else code.strip()))
    return body, lines


def find_positions(body):
    """The skinned positions: [[x, y, z, w]] as value names, in program order."""
    op = {name: rhs for _, name, rhs in body if name}
    order = {name: k for k, (_, name, _) in enumerate(body) if name}
    fdivs = {name for name, rhs in op.items() if rhs.startswith('fdiv')}
    users = {}
    for name, rhs in op.items():
        for o in OPND.findall(rhs):
            users.setdefault(o, []).append(name)
    memo = {}

    def bones(n, depth=0):
        if n in memo:
            return memo[n]
        rhs = op.get(n, '')
        out = frozenset()
        if depth < 64 and rhs.startswith('fmul'):
            ops = OPND.findall(rhs)
            hit = [o for o in ops if o in fdivs]
            other = [o for o in ops if o not in fdivs]
            # a position term: weight times mul(localPosition, bone), a computed value. CompileSkinningMatrix's
            # terms are weight times a matrix element read straight from the bone buffer (extractvalue): not these.
            # (and only a dot product: an fadd or an FMad - ApplySkinning's wetness sums (1 - extra) * weight, an
            # fsub, over the same four weights)
            o = op.get(other[0], '') if other else ''
            if hit and (o.startswith('fadd') or 'dx.op.tertiary.f32(i32 46,' in o):
                out = frozenset(hit)
        elif depth < 64 and rhs.startswith('fadd'):
            s = set()
            for o in OPND.findall(rhs):
                s |= bones(o, depth + 1)
            out = frozenset(s)
        memo[n] = out
        return out

    # The second form (fast-math shaders that blend the matrix first, then multiply the position with it once):
    # mat(n) = the weights in a blended matrix element (a sum of weight times an element read from the bone buffer);
    # pos(n) = the weights in a position component computed from such elements: x*M0 + y*M1 + z*M2 + M3, as fmul,
    # FMad (dx.op.tertiary 46: a*b + c, the sum goes on through c) and fadd.
    mmemo, pmemo = {}, {}

    def mat(n, depth=0):
        if n in mmemo:
            return mmemo[n]
        rhs = op.get(n, '')
        out = frozenset()
        if depth < 64 and rhs.startswith('fmul'):
            ops = OPND.findall(rhs)
            hit = [o for o in ops if o in fdivs]
            other = [o for o in ops if o not in fdivs]
            if hit and other and op.get(other[0], '').startswith('extractvalue'):
                out = frozenset(hit)
        elif depth < 64 and rhs.startswith('fadd'):
            s = set()
            for o in OPND.findall(rhs):
                s |= mat(o, depth + 1)
            out = frozenset(s)
        mmemo[n] = out
        return out

    def fmad_ops(rhs):
        if 'dx.op.tertiary.f32(i32 46,' not in rhs:
            return None
        ops = OPND.findall(rhs.split('(', 1)[1])
        return ops if len(ops) == 3 else None

    # the local position: input row 0 (POSITION) times the quantization scale plus its offset (cbuffer values); the
    # normal and tangent are multiplied with the same blended matrix and are left out by this
    local = {n for n, r in op.items() if r.startswith('call float @dx.op.loadInput.f32(i32 4, i32 0,')}
    from_cb = {n for n, r in op.items() if r.startswith('extractvalue %dx.types.CBufRet')}
    grew = True
    while grew:
        grew = False
        for n, r in op.items():
            if n in local or not (r.startswith('fmul') or r.startswith('fadd')):
                continue
            ops = OPND.findall(r)
            if ops and any(o in local for o in ops) and all(o in local or o in from_cb for o in ops):
                local.add(n)
                grew = True

    def pos(n, depth=0):
        if n in pmemo:
            return pmemo[n]
        rhs = op.get(n, '')
        out = set()
        if depth < 64:
            if rhs.startswith('fmul'):
                ops = OPND.findall(rhs)
                if any(o in local for o in ops):
                    for o in ops:
                        out |= mat(o)
            elif fmad_ops(rhs):
                a, b, c = fmad_ops(rhs)
                if a in local or b in local:
                    out |= mat(a) | mat(b)
                out |= pos(c, depth + 1)
            elif rhs.startswith('fadd') and not mat(n):
                for o in OPND.findall(rhs):
                    out |= pos(o, depth + 1) | mat(o)
        pmemo[n] = frozenset(out)
        return pmemo[n]

    def carries(u, n):
        """u takes n's sum further (an fadd of it, or an FMad with n as the added c)."""
        rhs = op.get(u, '')
        if rhs.startswith('fadd'):
            return True
        f = fmad_ops(rhs)
        return bool(f) and f[2] == n

    finals = []
    for name, rhs in op.items():
        if rhs.startswith('fadd') and len(bones(name)) >= 4:
            # topmost: no fadd user carries the same bones further
            if any(op.get(u, '').startswith('fadd') and bones(u) >= bones(name) for u in users.get(name, [])):
                continue
            finals.append(name)
        elif (rhs.startswith('fadd') or fmad_ops(rhs)) and not bones(name) and len(pos(name)) >= 4:
            if any(carries(u, name) and pos(u) >= pos(name) for u in users.get(name, [])):
                continue
            finals.append(name)
    bones_of = lambda n: bones(n) or pos(n)  # noqa: E731
    finals.sort(key=lambda n: order[n])
    positions, i = [], 0
    while i + 3 < len(finals):
        group = finals[i:i + 4]
        if len({bones_of(n) for n in group}) == 1:
            positions.append(group)
            i += 4
        else:
            i += 1
    return positions


def f32hex(v):
    """An LLVM float constant: the double of the nearest float, in hex."""
    d = struct.unpack('<d', struct.pack('<d', struct.unpack('<f', struct.pack('<f', v))[0]))[0]
    return '0x%016X' % struct.unpack('<Q', struct.pack('<d', d))[0]


CB_REGISTER, CB_SPACE = 13, 7  # TW3SE's constant buffer: b13, space7 (appended to the game's root signature)


def add_cbv(lines, size, register=None, type_name='%tw3se.SkinConsts'):
    """Declares a constant buffer of TW3SE's (b<register>, space7, `size` bytes; b13 unless given) in the shader's
    metadata and returns its range id (the list of lines is changed in place)."""
    register = CB_REGISTER if register is None else register
    ids = [int(m.group(1)) for m in (re.match(r'^!(\d+) = ', l) for l in lines) if m]
    nxt = max(ids) + 1
    res_line = next(i for i, l in enumerate(lines) if l.startswith('!dx.resources = '))
    tup = re.search(r'!\{!(\d+)\}', lines[res_line]).group(1)
    ti = next(i for i, l in enumerate(lines) if l.startswith(f'!{tup} = !{{'))
    elems = [e.strip() for e in lines[ti].split('= !{', 1)[1].rstrip('}').split(',')]
    record = nxt
    if elems[2] == 'null':
        rid = 0
        lst = nxt + 1
        elems[2] = f'!{lst}'
        lines[ti] = f'!{tup} = !{{' + ', '.join(elems) + '}'
        lines.append(f'!{lst} = !{{!{record}}}')
    else:
        li = next(i for i, l in enumerate(lines) if l.startswith(f'{elems[2]} = !{{'))
        inner = lines[li].split('= !{', 1)[1].rstrip('}')
        rid = len([e for e in inner.split(',') if e.strip()])
        lines[li] = lines[li].rstrip('}') + f', !{record}}}'
    lines.append(f'!{record} = !{{i32 {rid}, {type_name}* undef, !"", i32 {CB_SPACE}, i32 {register}, '
                 f'i32 1, i32 {size}, null}}')
    di = next(i for i, l in enumerate(lines) if l.startswith('define void @'))
    lines.insert(di, f'{type_name} = type {{ [{max(1, size // 16)} x <4 x float>] }}')
    lines.insert(di + 1, '')
    return rid


SLOT_REGISTER = 12  # the root constant with the slot of the mesh drawn (b12, space7), set by TW3SE before each draw
SLOTS = 8           # registered meshes; slot 0 = none (the table's row 0 stays zero)


def ensure_declare(lines, decl):
    """Adds a `declare` line for a dx.op function the shader does not use yet."""
    name = decl.split('@', 1)[1].split('(', 1)[0]
    if any(l.startswith('declare') and f'@{name}(' in l for l in lines):
        return
    di = max(i for i, l in enumerate(lines) if l.startswith('declare'))
    lines.insert(di + 1, decl)


CAPS = 32  # capsules per slot (skinhook.cpp kCaps)
CAP_ROWS = 3  # rows per capsule and time: a.xyz + radius, b.xyz + strength, falloff + mode
# The table at b13 (skinhook.cpp writes it the same way): rows 0..8 the counts of slot s (x: this frame, y: the frame
# before), then per slot, capsule, time (0 this frame, 1 the frame before) three rows: a.xyz + radius, b.xyz +
# strength, falloff + mode (the rest 0).
CAP_ROW0 = SLOTS + 1
TABLE_ROWS = CAP_ROW0 + (SLOTS + 1) * CAPS * CAP_ROWS * 2

# Per-vertex weights (a zone mask, stiffness): StructuredBuffer<float> at t14 space7, element 0 always 1. b12's y is
# where the drawn chunk's weights start (~0: none, element 0 then); the vertex id (SV_VertexID, added to the shader's
# inputs) is the vertex within the chunk (the game binds stream 0 at the chunk, base vertex 0). The push times it.
WEIGHT_REGISTER = 14
WEIGHT_CAPACITY = 1 << 20  # floats (skinhook.cpp kWeightCapacity); the index is held below it
MUSH_REGISTER = 15  # Delta Mush: u15 (skinned positions out) and t15 (offsets in), space7, at b12's z + vertex id

LABEL = re.compile(r'^; <label>:(\d+)')
NAMED_LABEL = re.compile(r'^([\w.]+):')
PHI_PRED = re.compile(r'(\[ [^\]]+, )(%[\w.]+)( \])')


class Emit:
    """IR lines with fresh names (%tw3se_<prefix><n>)."""

    def __init__(self, prefix):
        self.prefix, self.n, self.lines = prefix, 0, []

    def __call__(self, rhs):
        self.n += 1
        name = f'%tw3se_{self.prefix}{self.n}'
        self.lines.append(f'  {name} = {rhs}')
        return name


def capsule_code(e, px, py, pz, time, block):
    """The push of the slot's capsules on (px, py, pz), times the vertex's weight: one branch per capsule (a capsule
    past the slot's count costs a compare). Starts in `block`; returns the new x, y, z and the block it ends in.
    Per capsule and time three rows: a.xyz + radius, b.xyz + strength, falloff + mode. d = distance to the segment ab.
    Out (mode >= 0): d becomes the smooth maximum of d and the radius over the falloff k (k = 0: out to the surface).
    In (mode < 0): a vertex outside, within k of the surface, is drawn to it (d' = r + (d - r) * ((d - r) / k)^2).
    The vertex moves by (d' - d) * strength * weight along the direction from the segment."""
    eps8, eps5, zero, quarter = f32hex(1e-8), f32hex(1e-5), f32hex(0.0), f32hex(0.25)
    has = e('icmp ne i32 %tw3se_wbase, -1')
    i0 = e('add i32 %tw3se_wbase, %tw3se_vid')
    i1 = e(f'select i1 {has}, i32 {i0}, i32 0')
    big = e(f'icmp ugt i32 {i1}, {WEIGHT_CAPACITY - 1}')
    wi = e(f'select i1 {big}, i32 0, i32 {i1}')
    wl = e(f'call %dx.types.ResRet.f32 @dx.op.rawBufferLoad.f32(i32 139, %dx.types.Handle %tw3se_wh, i32 {wi}, i32 0, '
           'i8 1, i32 4)')
    weight = e(f'extractvalue %dx.types.ResRet.f32 {wl}, 0')
    cnt = e(f'extractvalue %dx.types.CBufRet.i32 %tw3se_cnt, {time}')
    base = e(f'add i32 %tw3se_capbase, {time * CAP_ROWS}')
    tag = e.prefix
    for c in range(CAPS):
        do, join = f'tw3se_{tag}c{c}', f'tw3se_{tag}j{c}'
        on = e(f'icmp ult i32 {c}, {cnt}')
        e.lines.append(f'  br i1 {on}, label %{do}, label %{join}')
        e.lines.append('')
        e.lines.append(f'{do}:')
        ra = e(f'add i32 {base}, {c * CAP_ROWS * 2}')
        rb = e(f'add i32 {ra}, 1')
        rc = e(f'add i32 {ra}, 2')
        la = e(f'call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %tw3se_cb, i32 {ra})')
        lb = e(f'call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %tw3se_cb, i32 {rb})')
        lc = e(f'call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %tw3se_cb, i32 {rc})')
        a = [e(f'extractvalue %dx.types.CBufRet.f32 {la}, {k}') for k in range(4)]
        b = [e(f'extractvalue %dx.types.CBufRet.f32 {lb}, {k}') for k in range(4)]
        kf = e(f'extractvalue %dx.types.CBufRet.f32 {lc}, 0')
        mode = e(f'extractvalue %dx.types.CBufRet.f32 {lc}, 1')
        ab = [e(f'fsub float {b[k]}, {a[k]}') for k in range(3)]
        p = (px, py, pz)
        ap = [e(f'fsub float {p[k]}, {a[k]}') for k in range(3)]
        d1 = e(f'fmul float {ap[0]}, {ab[0]}')
        d1 = e(f'call float @dx.op.tertiary.f32(i32 46, float {ap[1]}, float {ab[1]}, float {d1})')
        d1 = e(f'call float @dx.op.tertiary.f32(i32 46, float {ap[2]}, float {ab[2]}, float {d1})')
        d2 = e(f'fmul float {ab[0]}, {ab[0]}')
        d2 = e(f'call float @dx.op.tertiary.f32(i32 46, float {ab[1]}, float {ab[1]}, float {d2})')
        d2 = e(f'call float @dx.op.tertiary.f32(i32 46, float {ab[2]}, float {ab[2]}, float {d2})')
        d2 = e(f'call float @dx.op.binary.f32(i32 35, float {d2}, float {eps8})')
        t = e(f'fdiv float {d1}, {d2}')
        t = e(f'call float @dx.op.unary.f32(i32 7, float {t})')  # saturate
        closest = [e(f'call float @dx.op.tertiary.f32(i32 46, float {ab[k]}, float {t}, float {a[k]})') for k in range(3)]
        v = [e(f'fsub float {p[k]}, {closest[k]}') for k in range(3)]
        dd = e(f'fmul float {v[0]}, {v[0]}')
        dd = e(f'call float @dx.op.tertiary.f32(i32 46, float {v[1]}, float {v[1]}, float {dd})')
        dd = e(f'call float @dx.op.tertiary.f32(i32 46, float {v[2]}, float {v[2]}, float {dd})')
        d = e(f'call float @dx.op.unary.f32(i32 24, float {dd})')  # sqrt
        r = a[3]
        kk = e(f'call float @dx.op.binary.f32(i32 35, float {kf}, float {eps5})')
        diff = e(f'fsub float {d}, {r}')
        # out: the smooth maximum of d and r over k
        adiff = e(f'call float @dx.op.unary.f32(i32 6, float {diff})')  # fabs
        h = e(f'fsub float {kf}, {adiff}')
        h = e(f'call float @dx.op.binary.f32(i32 35, float {h}, float {zero})')
        h = e(f'fdiv float {h}, {kk}')
        mx = e(f'call float @dx.op.binary.f32(i32 35, float {d}, float {r})')
        hh = e(f'fmul float {h}, {h}')
        hk = e(f'fmul float {kf}, {quarter}')
        dpo = e(f'call float @dx.op.tertiary.f32(i32 46, float {hh}, float {hk}, float {mx})')
        # in: drawn to the surface from within k outside it
        ti = e(f'fdiv float {diff}, {kk}')
        ti = e(f'call float @dx.op.unary.f32(i32 7, float {ti})')  # saturate
        tt = e(f'fmul float {ti}, {ti}')
        dpi = e(f'call float @dx.op.tertiary.f32(i32 46, float {diff}, float {tt}, float {r})')
        outside = e(f'fcmp fast ogt float {diff}, {zero}')
        dpi = e(f'select i1 {outside}, float {dpi}, float {d}')
        inward = e(f'fcmp fast olt float {mode}, {zero}')
        dp = e(f'select i1 {inward}, float {dpi}, float {dpo}')
        push = e(f'fsub float {dp}, {d}')
        push = e(f'fmul float {push}, {b[3]}')
        push = e(f'fmul float {push}, {weight}')
        dm = e(f'call float @dx.op.binary.f32(i32 35, float {d}, float {eps5})')
        k = e(f'fdiv float {push}, {dm}')
        nx, ny, nz = (e(f'call float @dx.op.tertiary.f32(i32 46, float {v[j]}, float {k}, float {(px, py, pz)[j]})')
                      for j in range(3))
        e.lines.append(f'  br label %{join}')
        e.lines.append('')
        e.lines.append(f'{join}:')
        merged = []
        for j, (old, new_) in enumerate(zip((px, py, pz), (nx, ny, nz))):
            name = f'%tw3se_{tag}m{c}{"xyz"[j]}'
            e.lines.append(f'  {name} = phi float [ {new_}, %{do} ], [ {old}, {block} ]')
            merged.append(name)
        px, py, pz = merged
        block = f'%{join}'
    return px, py, pz, block


def add_srv_float(lines, register, space=CB_SPACE):
    """Declares a StructuredBuffer<float> at t<register>, space7; returns its range id."""
    ids = [int(m.group(1)) for m in (re.match(r'^!(\d+) = ', l) for l in lines) if m]
    nxt = max(ids) + 1
    res_line = next(i for i, l in enumerate(lines) if l.startswith('!dx.resources = '))
    tup = re.search(r'!\{!(\d+)\}', lines[res_line]).group(1)
    ti = next(i for i, l in enumerate(lines) if l.startswith(f'!{tup} = !{{'))
    elems = [e.strip() for e in lines[ti].split('= !{', 1)[1].rstrip('}').split(',')]
    record, props = nxt, nxt + 1
    if elems[0] == 'null':
        rid, lst = 0, nxt + 2
        elems[0] = f'!{lst}'
        lines[ti] = f'!{tup} = !{{' + ', '.join(elems) + '}'
        lines.append(f'!{lst} = !{{!{record}}}')
    else:
        li = next(i for i, l in enumerate(lines) if l.startswith(f'{elems[0]} = !{{'))
        inner = lines[li].split('= !{', 1)[1].rstrip('}')
        rid = len([x for x in inner.split(',') if x.strip()])
        lines[li] = lines[li].rstrip('}') + f', !{record}}}'
    lines.append(f'!{record} = !{{i32 {rid}, %"class.StructuredBuffer<float>"* undef, !"", i32 {space}, i32 {register}, '
                 f'i32 1, i32 12, i32 0, !{props}}}')
    lines.append(f'!{props} = !{{i32 1, i32 4}}')
    if not any(l.startswith('%"class.StructuredBuffer<float>" = ') for l in lines):
        di = next(i for i, l in enumerate(lines) if l.startswith('define void @'))
        lines.insert(di, '%"class.StructuredBuffer<float>" = type { float }')
    return rid


def add_vertex_id(lines):
    """SV_VertexID added to the shader's inputs (after the last one, with the used mask DXC writes); the view-id state
    goes (it names the old count of inputs; the validator takes a shader without it). Returns the element id (the
    shader's own when it reads it already)."""
    ids = [int(m.group(1)) for m in (re.match(r'^!(\d+) = ', l) for l in lines) if m]
    nxt = max(ids) + 1
    ep = next(l for l in lines if l.startswith('!dx.entryPoints'))
    epid = re.search(r'!\{!(\d+)\}', ep).group(1)
    epl = next(l for l in lines if l.startswith(f'!{epid} = !{{'))
    sigs = re.findall(r'!(\d+)', epl.split('=', 1)[1])[0]  # (function, name, signatures, resources, props)
    sigl = next(l for l in lines if l.startswith(f'!{sigs} = !{{'))
    in_list = re.search(r'!\{!(\d+)', sigl.split('=', 1)[1]).group(1)
    inl_i = next(i for i, l in enumerate(lines) if l.startswith(f'!{in_list} = !{{'))
    elems = re.findall(r'!(\d+)', lines[inl_i].split('=', 1)[1])
    last_id, last_row, idx = -1, -1, None
    for el in elems:
        line = next(l for l in lines if l.startswith(f'!{el} = !{{'))
        f = [x.strip() for x in line.split('= !{', 1)[1].rstrip('}').split(',')]
        if f[1] == '!"SV_VertexID"':  # the shader reads it already
            return int(f[0].split()[1])
        last_id = max(last_id, int(f[0].split()[1]))
        last_row = max(last_row, int(f[8].split()[1]) + int(f[6].split()[1]) - 1)
        idx = idx or f[4]
    eid = last_id + 1
    lines[:] = [l for l in lines if not l.startswith('!dx.viewIdState')]
    inl_i = next(i for i, l in enumerate(lines) if l.startswith(f'!{in_list} = !{{'))
    lines[inl_i] = lines[inl_i].rstrip('}') + f', !{nxt}}}'
    lines.append(f'!{nxt} = !{{i32 {eid}, !"SV_VertexID", i8 5, i8 1, {idx}, i8 0, i32 1, i8 1, i32 {last_row + 1}, '
                 f'i8 0, !{nxt + 1}}}')
    lines.append(f'!{nxt + 1} = !{{i32 3, i32 1}}')
    return eid


LOAD_RE = re.compile(r'@dx\.op\.rawBufferLoad\.f32\(i32 139, %dx\.types\.Handle (%[\w.]+), i32 (%[\w.]+|\d+), '
                     r'i32 (\d+),')
EXT_RE = re.compile(r'extractvalue %dx\.types\.ResRet\.f32 (%[\w.]+), (\d+)$')


def dqs_inputs(body, comps):
    """What dual quaternion skinning needs of one skinned position (its x, y, z): per bone its weight (the fdiv: weight
    over the sum) and the four rows of its matrix (rawBufferLoad at 0, 16, 32, 48 bytes: p' = x*r0 + y*r1 + z*r2 + r3),
    and the local position. {'bones': [(weight, [row0, row1, row2, row3])] x4, 'local': [lx, ly, lz]}, or None."""
    op = {name: rhs for _, name, rhs in body if name}
    fdivs = {n for n, r in op.items() if r.startswith('fdiv')}
    loads = {}
    for n, r in op.items():
        m = LOAD_RE.search(r)
        if m:
            loads[n] = (m.group(1), m.group(2), int(m.group(3)))
    # the local position: input row 0 (POSITION), times and plus constant buffer values
    local = {n for n, r in op.items() if r.startswith('call float @dx.op.loadInput.f32(i32 4, i32 0,')}
    from_cb = {n for n, r in op.items() if r.startswith('extractvalue %dx.types.CBufRet')}
    grew = True
    while grew:
        grew = False
        for n, r in op.items():
            if n in local or not (r.startswith('fmul') or r.startswith('fadd')):
                continue
            ops = OPND.findall(r)
            if ops and any(o in local for o in ops) and all(o in local or o in from_cb for o in ops):
                local.add(n)
                grew = True

    def operands(r):
        if 'dx.op.tertiary.f32(i32 46,' in r:
            return OPND.findall(r.split('(', 1)[1])[-3:]
        if r.startswith(('fmul', 'fadd')):
            return OPND.findall(r)
        return []

    rmemo = {}

    def reach(n, depth=0):  # the loads n's value is made of (not past a weight or the local position)
        if n in rmemo:
            return rmemo[n]
        r = op.get(n, '')
        m = EXT_RE.match(r)
        out = set()
        if m and m.group(1) in loads:
            out = {m.group(1)}
        elif depth < 64 and n not in fdivs and n not in local:
            for o in operands(r):
                out |= reach(o, depth + 1)
        rmemo[n] = out
        return out

    weight_loads, lpos, seen = {}, {}, set()

    def walk(n, depth=0):
        if n in seen or depth > 96 or n in fdivs or n in local:
            return
        seen.add(n)
        r = op.get(n, '')
        ops = operands(r)
        mul = ops[:2] if 'dx.op.tertiary.f32(i32 46,' in r else (ops if r.startswith('fmul') else [])
        if len(mul) == 2:
            a, b = mul
            for f, other in ((a, b), (b, a)):
                if f in fdivs:
                    weight_loads.setdefault(f, set()).update(reach(other))
            for lv, other in ((a, b), (b, a)):
                # the local position: a factor made of no bone (the input in a vertex shader, the vertex buffer's
                # unpacked value in the ray tracing compute shader), times an element of one row of a bone
                if lv in local or (lv not in fdivs and not reach(lv)):
                    rows = {loads[c][2] for c in reach(other)}
                    if len(rows) == 1 and min(rows) // 16 < 3:
                        lpos.setdefault(min(rows) // 16, lv)
        for o in ops:
            walk(o, depth + 1)

    for c in comps:
        walk(c)
    if len(weight_loads) != 4 or len(lpos) != 3:
        return None
    bones = []
    for f, calls in weight_loads.items():
        keys = {(loads[c][0], loads[c][1]) for c in calls}
        if len(keys) != 1:
            return None
        key = keys.pop()
        rows = {}
        for c, (h, i, off) in loads.items():
            if (h, i) == key:
                rows.setdefault(off, c)
        if sorted(rows) != [0, 16, 32, 48]:
            return None
        bones.append((f, [rows[0], rows[16], rows[32], rows[48]]))
    return {'bones': bones, 'local': [lpos[0], lpos[1], lpos[2]]}


def dqs_code(e, inp):
    """Dual quaternion skinning (Kavan 2007) of the local position with the four bones: per bone the rotation as a
    quaternion (its 3x3 part over the bone's mean scale, branch free), the translation as the dual part; blended in
    the hemisphere of the heaviest bone, normalized, applied; the bones' scales blended linearly. Returns x, y, z."""
    F = f32hex
    mul = lambda a, b: e(f'fmul float {a}, {b}')  # noqa: E731
    add = lambda a, b: e(f'fadd float {a}, {b}')  # noqa: E731
    sub = lambda a, b: e(f'fsub float {a}, {b}')  # noqa: E731
    sqrt = lambda a: e(f'call float @dx.op.unary.f32(i32 24, float {a})')  # noqa: E731
    fmax = lambda a, b: e(f'call float @dx.op.binary.f32(i32 35, float {a}, float {b})')  # noqa: E731
    sel = lambda c, a, b: e(f'select i1 {c}, float {a}, float {b}')  # noqa: E731

    def dot3(a, b):
        return add(add(mul(a[0], b[0]), mul(a[1], b[1])), mul(a[2], b[2]))

    def cross(a, b):
        return [sub(mul(a[1], b[2]), mul(a[2], b[1])), sub(mul(a[2], b[0]), mul(a[0], b[2])),
                sub(mul(a[0], b[1]), mul(a[1], b[0]))]

    half, one, zero, third, two, tiny = F(0.5), F(1.0), F(0.0), F(1.0 / 3.0), F(2.0), F(1e-8)
    qs, ds, ws, ts = [], [], [], []
    ms, rs = [], []
    for w, rows in inp['bones']:
        m = [[e(f'extractvalue %dx.types.ResRet.f32 {rows[r]}, {c}') for c in range(3)] for r in range(4)]
        # the bone's turn: its 3x3 part made orthonormal (Gram-Schmidt on the rows), so a scale, also an uneven one,
        # gives no turn of its own; what the scale does stays in the game's blend (see below)
        def unit(v):
            li = e(f'fdiv float {one}, {fmax(sqrt(dot3(v, v)), tiny)}')
            return [mul(v[c], li) for c in range(3)]

        u0 = unit(m[0])
        d01 = dot3(m[1], u0)
        u1 = unit([sub(m[1][c], mul(d01, u0[c])) for c in range(3)])
        u2 = cross(u0, u1)
        R = [u0, u1, u2]
        a = lambda i, j: R[j][i]  # noqa: E731  (column convention: a_ij = M[j][i])
        a00, a11, a22 = a(0, 0), a(1, 1), a(2, 2)

        def part(sx, sy, sz):
            t = add(add(add(one, mul(a00, F(sx))), mul(a11, F(sy))), mul(a22, F(sz)))
            return mul(sqrt(fmax(t, zero)), half)

        def signed(v, d):
            neg = e(f'fcmp olt float {d}, {zero}')
            return sel(neg, sub(zero, v), v)

        qw = part(1, 1, 1)
        qx = signed(part(1, -1, -1), sub(a(2, 1), a(1, 2)))
        qy = signed(part(-1, 1, -1), sub(a(0, 2), a(2, 0)))
        qz = signed(part(-1, -1, 1), sub(a(1, 0), a(0, 1)))
        qn = sqrt(add(dot3([qx, qy, qz], [qx, qy, qz]), mul(qw, qw)))
        qi = e(f'fdiv float {one}, {fmax(qn, tiny)}')
        q = [mul(qx, qi), mul(qy, qi), mul(qz, qi), mul(qw, qi)]
        t = m[3]
        # dual part: 0.5 * (t, 0) * q
        tq = cross(t, q[:3])
        d = [mul(add(mul(q[3], t[k]), tq[k]), half) for k in range(3)] + [mul(dot3(t, q[:3]), F(-0.5))]
        qs.append(q)
        ds.append(d)
        ws.append(w)
        ts.append(t)
        ms.append(m)
        rs.append(R)
    # The hemisphere per bone, the same for every vertex: the rest pose's (w >= 0). A choice per vertex (its heaviest
    # bone) makes steps between neighbours whose heaviest bones differ; this one stays continuous over the mesh and
    # agrees with signs along the bone chain while no bone is turned half a turn or more from its rest pose.
    def rotate(q, v):  # v turned by the unit quaternion q
        cv = cross(q[:3], v)
        inner = [add(cv[k], mul(q[3], v[k])) for k in range(3)]
        c2 = cross(q[:3], inner)
        return [add(v[k], mul(c2[k], two)) for k in range(3)]

    v = inp['local']
    b0, be, rigid = [zero] * 4, [zero] * 4, [zero] * 3
    for k in range(4):
        neg = e(f'fcmp olt float {qs[k][3]}, {zero}')
        wk = sel(neg, sub(zero, ws[k]), ws[k])
        b0 = [add(b0[c], mul(wk, qs[k][c])) for c in range(4)]
        be = [add(be[c], mul(wk, ds[k][c])) for c in range(4)]
        # the linear blend of the rigid bones (their quaternion and translation): what the bones' scale adds is the
        # game's blend minus this, kept as it is
        rv = rotate(qs[k], v)
        rigid = [add(rigid[c], mul(ws[k], add(rv[c], ts[k][c]))) for c in range(3)]
    n = sqrt(add(dot3(b0[:3], b0[:3]), mul(b0[3], b0[3])))
    ni = e(f'fdiv float {one}, {fmax(n, tiny)}')
    c0 = [mul(b0[c], ni) for c in range(4)]
    ce = [mul(be[c], ni) for c in range(4)]
    rot = rotate(c0, v)
    ct = cross(c0[:3], ce[:3])
    out = []
    for k in range(3):
        tr = mul(add(sub(mul(c0[3], ce[k]), mul(ce[3], c0[k])), ct[k]), two)
        scaled = sub(inp['lbs'][k], rigid[k]) if inp.get('lbs') else zero
        out.append(add(add(rot[k], tr), scaled))
    # the normal's matrix (row form, n' = n . N): the blended turn plus what the bones' scale adds, as for the position
    x2, y2, z2 = mul(c0[0], c0[0]), mul(c0[1], c0[1]), mul(c0[2], c0[2])
    xy, xz, yz = mul(c0[0], c0[1]), mul(c0[0], c0[2]), mul(c0[1], c0[2])
    xw, yw, zw = mul(c0[0], c0[3]), mul(c0[1], c0[3]), mul(c0[2], c0[3])
    A = [[sub(one, mul(two, add(y2, z2))), mul(two, sub(xy, zw)), mul(two, add(xz, yw))],
         [mul(two, add(xy, zw)), sub(one, mul(two, add(x2, z2))), mul(two, sub(yz, xw))],
         [mul(two, sub(xz, yw)), mul(two, add(yz, xw)), sub(one, mul(two, add(x2, y2)))]]
    N = [[A[j][i] for j in range(3)] for i in range(3)]
    for k in range(4):
        for i in range(3):
            for j in range(3):
                N[i][j] = add(N[i][j], mul(ws[k], sub(ms[k][i][j], rs[k][i][j])))
    inp['normal'] = [N[i][j] for i in range(3) for j in range(3)]
    if os.environ.get('TW3SE_DQS_DEBUG'):  # (a check: the branch taken lifts the mesh by 0.3 m)
        out[2] = add(out[2], F(0.3))
    return out


def patch_capsules(text):
    """Capsules of the slot of the mesh drawn push its skinned vertices out to their surface (this frame's position
    with this frame's capsules, the previous one's with the previous ones), behind a branch: slot 0 costs nothing."""
    body, lines = parse(text)
    positions = find_positions(body)
    if not positions:
        raise SystemExit('no skinned position found')
    where = {name: idx for idx, name, _ in body if name}
    # the split points: after the last of x, y, z of each position; none of them used before that
    splits = {}
    for p, (x, y, z, w) in enumerate(positions):
        last = max(where[x], where[y], where[z])
        for v in (x, y, z):
            for i in range(where[v] + 1, last + 1):
                if re.search(re.escape(v) + r'(?![\w.])', lines[i].split(';', 1)[0].split('=', 1)[-1]):
                    raise SystemExit(f'{v} is used before the position is complete (line {i + 1}): not patched')
        splits[last] = (p, x, y, z)
    out = []
    rename = {}        # component -> its value after the push
    pred_map = {}      # a block split here -> the block its end now belongs to (for later phis)
    block = '%0'
    for i, line in enumerate(lines):
        m = LABEL.match(line) or NAMED_LABEL.match(line)
        if m and not line.startswith('define') and i > body[0][0] - 1:
            block = '%' + m.group(1)
        code, sep, comment = line.partition(';')
        if rename:
            dm = DEF.match(code.rstrip())
            own = dm.group(1) if dm else None
            code = OPND.sub(lambda mo: rename.get(mo.group(0), mo.group(0)) if mo.group(0) != own else mo.group(0), code)
        if ' phi ' in code and pred_map:
            def pred(mo):
                lab = mo.group(2)
                while lab in pred_map:
                    lab = pred_map[lab]
                return mo.group(1) + lab + mo.group(3)
            code = PHI_PRED.sub(pred, code)
        out.append(code + sep + comment if sep else code)
        if i in splits:
            p, x, y, z = splits[i]
            time = 1 if (len(positions) == 2 and p == 1) else 0
            e = Emit(f'p{p}_')
            do, join = f'tw3se_p{p}_do', f'tw3se_p{p}_join'
            start = f'%{do}'
            # Dual quaternion skinning (the slot's row w, bit 17): the position skinned anew from the same bones and
            # weights, in place of the game's linear blend; Delta Mush and the capsules work on it
            dqi = dqs_inputs(body, (x, y, z))
            bx, by, bz = x, y, z
            if dqi:
                qdo, qj = f'tw3se_p{p}_qdo', f'tw3se_p{p}_qj'
                q0 = e('and i32 %tw3se_cntw, 131072')
                qon = e(f'icmp ne i32 {q0}, 0')
                e.lines += [f'  br i1 {qon}, label %{qdo}, label %{qj}', '', f'{qdo}:']
                dqi['lbs'] = [x, y, z]
                qx, qy, qz = dqs_code(e, dqi)
                e.lines += [f'  br label %{qj}', '', f'{qj}:']
                bx, by, bz = (e(f'phi float [ {qv}, %{qdo} ], [ {ov}, {start} ]') for qv, ov in ((qx, x), (qy, y), (qz, z)))
                nq = [e(f'phi float [ {nv_}, %{qdo} ], [ 0.000000e+00, {start} ]') for nv_ in dqi['normal']]
                nqon = e(f'phi i1 [ true, %{qdo} ], [ false, {start} ]')
                start = f'%{qj}'
            else:
                print(f'  dqs: position {p} not understood, left linear', file=sys.stderr)
            pre = start
            if time == 0:  # Delta Mush: the skinned position out (u15), the mush offset in (t15)
                dmdo, dmj = f'tw3se_p{p}_dmdo', f'tw3se_p{p}_dmj'
                on = e('icmp ne i32 %tw3se_dmz, -1')
                e.lines += [f'  br i1 {on}, label %{dmdo}, label %{dmj}', '', f'{dmdo}:']
                di = e('add i32 %tw3se_dmz, %tw3se_vid')
                at = e(f'shl i32 {di}, 4')
                dq = e(f'add i32 {di}, %tw3se_dmw')
                atq = e(f'shl i32 {dq}, 4')
                e.lines.append(f'  call void @dx.op.rawBufferStore.f32(i32 140, %dx.types.Handle %tw3se_qh, i32 {atq}, '
                               f'i32 undef, float {bx}, float {by}, float {bz}, float 1.000000e+00, i8 15, i32 4)')
                ol = e(f'call %dx.types.ResRet.f32 @dx.op.rawBufferLoad.f32(i32 139, %dx.types.Handle %tw3se_oh, '
                       f'i32 {at}, i32 undef, i8 7, i32 4)')
                o = [e(f'extractvalue %dx.types.ResRet.f32 {ol}, {k}') for k in range(3)]
                m = [e(f'fadd float {(bx, by, bz)[k]}, {o[k]}') for k in range(3)]
                e.lines += [f'  br label %{dmj}', '', f'{dmj}:']
                merged = []
                for k, (old, new_) in enumerate(zip((bx, by, bz), m)):
                    name = f'%tw3se_p{p}_dm{"xyz"[k]}'
                    e.lines.append(f'  {name} = phi float [ {new_}, %{dmdo} ], [ {old}, {pre} ]')
                    merged.append(name)
                cx, cy, cz = merged
                start = f'%{dmj}'
            else:  # the frame before's position (motion vectors): the same offset added, nothing written
                dmdo, dmj = f'tw3se_p{p}_dmdo', f'tw3se_p{p}_dmj'
                on = e('icmp ne i32 %tw3se_dmz, -1')
                e.lines += [f'  br i1 {on}, label %{dmdo}, label %{dmj}', '', f'{dmdo}:']
                di = e('add i32 %tw3se_dmz, %tw3se_vid')
                at = e(f'shl i32 {di}, 4')
                ol = e(f'call %dx.types.ResRet.f32 @dx.op.rawBufferLoad.f32(i32 139, %dx.types.Handle %tw3se_oh, '
                       f'i32 {at}, i32 undef, i8 7, i32 4)')
                o = [e(f'extractvalue %dx.types.ResRet.f32 {ol}, {k}') for k in range(3)]
                m = [e(f'fadd float {(bx, by, bz)[k]}, {o[k]}') for k in range(3)]
                e.lines += [f'  br label %{dmj}', '', f'{dmj}:']
                merged = []
                for k, (old, new_) in enumerate(zip((bx, by, bz), m)):
                    name = f'%tw3se_p{p}_dm{"xyz"[k]}'
                    e.lines.append(f'  {name} = phi float [ {new_}, %{dmdo} ], [ {old}, {pre} ]')
                    merged.append(name)
                cx, cy, cz = merged
                start = f'%{dmj}'
            nx, ny, nz, last = capsule_code(e, cx, cy, cz, time, start)
            out.append(f'  %tw3se_p{p}_go = icmp ne i32 %tw3se_slot, 0')
            out.append(f'  br i1 %tw3se_p{p}_go, label %{do}, label %{join}')
            out.append('')
            out.append(f'{do}:')
            out.extend(e.lines)
            pushes = []
            for c, cv, nv in zip('xyz', (cx, cy, cz), (nx, ny, nz)):  # the capsules' push alone (cap_normals)
                out.append(f'  %tw3se_p{p}_push{c} = fsub float {nv}, {cv}')
                pushes.append(f'%tw3se_p{p}_push{c}')
            out.append(f'  br label %{join}')
            out.append('')
            out.append(f'{join}:')
            for c, v, nv in zip('xyz', (x, y, z), (nx, ny, nz)):
                out.append(f'  %tw3se_p{p}{c} = phi float [ {nv}, {last} ], [ {v}, {block} ]')
                rename[v] = f'%tw3se_p{p}{c}'
            for c, pv in zip('xyz', pushes):
                out.append(f'  %tw3se_p{p}d{c} = phi float [ {pv}, {last} ], [ 0.000000e+00, {block} ]')
            if dqi:  # the DQS normal matrix and whether it holds (dqs_normals)
                for i, nv_ in enumerate(nq):
                    out.append(f'  %tw3se_p{p}N{i} = phi float [ {nv_}, {last} ], [ 0.000000e+00, {block} ]')
                out.append(f'  %tw3se_p{p}Nq = phi i1 [ {nqon}, {last} ], [ false, {block} ]')
            pred_map[block] = f'%{join}'
            block = f'%{join}'
    # the constant buffers, the weights, the vertex id; the slot and the counts at the function's start
    rid_slot = add_cbv(out, 16, SLOT_REGISTER, '%tw3se.SkinSlot')
    rid_tab = add_cbv(out, 16 * TABLE_ROWS)
    rid_w = add_srv_float(out, WEIGHT_REGISTER)
    rid_q = add_uav_raw(out, MUSH_REGISTER)
    rid_o = add_srv_raw(out, MUSH_REGISTER)
    add_shader_flag(out, 1 << 16)  # UAVs at every stage
    ensure_declare(out, 'declare void @dx.op.rawBufferStore.f32(i32, %dx.types.Handle, i32, i32, float, float, float, '
                        'float, i8, i32) #1')
    vid_id = add_vertex_id(out)
    ensure_declare(out, 'declare %dx.types.ResRet.f32 @dx.op.rawBufferLoad.f32(i32, %dx.types.Handle, i32, i32, i8, i32) #1')
    ensure_declare(out, 'declare i32 @dx.op.loadInput.i32(i32, i32, i32, i8, i32) #0')
    if not any(l.startswith('%dx.types.ResRet.f32 = ') for l in out):
        ti = next(i for i, l in enumerate(out) if l.startswith('%dx.types.CBufRet.f32 = '))
        out.insert(ti + 1, '%dx.types.ResRet.f32 = type { float, float, float, float, i32 }')
    ensure_declare(out, 'declare %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32, %dx.types.Handle, i32) #2')
    ensure_declare(out, 'declare float @dx.op.binary.f32(i32, float, float) #0')
    ensure_declare(out, 'declare float @dx.op.unary.f32(i32, float) #0')
    ensure_declare(out, 'declare float @dx.op.tertiary.f32(i32, float, float, float) #0')
    if not any(l.startswith('%dx.types.CBufRet.i32 = ') for l in out):
        ti = next(i for i, l in enumerate(out) if l.startswith('%dx.types.CBufRet.f32 = '))
        out.insert(ti + 1, '%dx.types.CBufRet.i32 = type { i32, i32, i32, i32 }')
    di = next(i for i, l in enumerate(out) if l.startswith('define void @'))
    out[di + 1:di + 1] = [
        f'  %tw3se_sh = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 {rid_slot}, i32 {SLOT_REGISTER}, '
        'i1 false)',
        f'  %tw3se_cb = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 {rid_tab}, i32 {CB_REGISTER}, '
        'i1 false)',
        '  %tw3se_sr = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %tw3se_sh, i32 0)',
        '  %tw3se_slot0 = extractvalue %dx.types.CBufRet.i32 %tw3se_sr, 0',
        '  %tw3se_wbase = extractvalue %dx.types.CBufRet.i32 %tw3se_sr, 1',
        '  %tw3se_dmz = extractvalue %dx.types.CBufRet.i32 %tw3se_sr, 2',
        '  %tw3se_dmw = extractvalue %dx.types.CBufRet.i32 %tw3se_sr, 3',
        f'  %tw3se_wh = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 0, i32 {rid_w}, i32 {WEIGHT_REGISTER}, i1 false)',
        f'  %tw3se_qh = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 {rid_q}, i32 {MUSH_REGISTER}, i1 false)',
        f'  %tw3se_oh = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 0, i32 {rid_o}, i32 {MUSH_REGISTER}, i1 false)',
        f'  %tw3se_vid = call i32 @dx.op.loadInput.i32(i32 4, i32 {vid_id}, i32 0, i8 0, i32 undef)',
        f'  %tw3se_big = icmp ugt i32 %tw3se_slot0, {SLOTS}',
        '  %tw3se_slot = select i1 %tw3se_big, i32 0, i32 %tw3se_slot0',
        '  %tw3se_cnt = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %tw3se_cb, '
        'i32 %tw3se_slot)',
        '  %tw3se_cntw = extractvalue %dx.types.CBufRet.i32 %tw3se_cnt, 3',
        f'  %tw3se_capb0 = mul i32 %tw3se_slot, {CAPS * CAP_ROWS * 2}',
        f'  %tw3se_capbase = add i32 %tw3se_capb0, {CAP_ROW0}',
    ]
    return cap_normals('\n'.join(out) + '\n'), positions


CAP_NORMAL_FULL = 0.01  # metres of push that turn the normal all the way to the push's line


def cap_normals(text):
    """The world normal the vertex shader writes, tilted with the capsules' push (see patch_capsules): the output row
    whose x, y, z are normalize(n.x * M0 + n.y * M1 + n.z * M2) with n from the NORMAL input, M = bone x LocalToWorld
    multiplied out in the shader; the push turned into the world with LocalToWorld's columns read from M. A shader
    without that pattern comes back as it was."""
    if '%tw3se_p0dx' not in text:
        return text
    body, lines = parse(text)
    defs = {name: (idx, rhs) for idx, name, rhs in body if name}
    elems, inside = [], False
    for line in lines:
        if line.startswith('; Input signature'):
            inside = True
            continue
        if inside and line.startswith('; Output signature'):
            break
        m = re.match(r';\s+([A-Z_]+)\s+\d+\s+\S+\s+\d+\s', line) if inside else None
        if m and m.group(1) != 'Name':
            elems.append(m.group(1))
    if 'NORMAL' not in elems:
        return text
    nid = elems.index('NORMAL')

    def is_load(v):
        return bool(re.match(rf'call float @dx\.op\.loadInput\.f32\(i32 4, i32 {nid}, ',
                             defs.get(v, (0, ''))[1]))

    def from_normal(v):
        """v = NORMAL * 2 - 1 straight from the input (the bitangent, a cross product of n and t, is not taken)."""
        d = defs.get(v, (0, ''))[1]
        m = re.match(r'fadd fast float (%[\w.]+), -1\.000000e\+00$', d)
        if m:
            m2 = re.match(r'fmul fast float (%[\w.]+), 2\.000000e\+00$', defs.get(m.group(1), (0, ''))[1])
            return bool(m2) and is_load(m2.group(1))
        m = re.match(r'call float @dx\.op\.tertiary\.f32\(i32 46, float (%[\w.]+), float 2\.000000e\+00, '
                     r'float -1\.000000e\+00\)$', d)
        return bool(m) and is_load(m.group(1))

    def products(v):
        out = []
        while True:
            d = defs.get(v, (0, ''))[1]
            m = re.match(r'call float @dx\.op\.tertiary\.f32\(i32 46, float (%[\w.]+), float (%[\w.]+), '
                         r'float (%[\w.]+)\)', d)
            if m:
                out.append(m.group(1, 2))
                v = m.group(3)
                continue
            m = re.match(r'fmul (?:fast )?float (%[\w.]+), (%[\w.]+)$', d)
            if m:
                out.append(m.group(1, 2))
                return out[::-1]  # the innermost first
            return None

    stored = {}  # a value -> the lines that store it
    for idx, name, rhs in body:
        m = re.match(r'call void @dx\.op\.storeOutput\.f32\(i32 5, i32 \d+, i32 0, i8 \d, float (%[\w.]+)\)', rhs)
        if m:
            stored.setdefault(m.group(1), []).append(idx)
    users = {}  # a value -> [(product, other factor)]
    for idx, name, rhs in body:
        m = name and re.match(r'fmul fast float (%[\w.]+), (%[\w.]+)$', rhs)
        if m:
            users.setdefault(m.group(1), []).append((name, m.group(2)))
            users.setdefault(m.group(2), []).append((name, m.group(1)))
    for idx, name, rhs in body:
        m = name and re.match(r'call float @dx\.op\.unary\.f32\(i32 25, float (%[\w.]+)\)$', rhs)
        if not m:
            continue
        dm = re.match(r'call float @dx\.op\.dot3\.f32\(i32 55, float (%[\w.]+), float (%[\w.]+), float (%[\w.]+), ',
                      defs.get(m.group(1), (0, ''))[1])
        if not dm:
            continue
        xs = list(dm.group(1, 2, 3))
        nv = []
        for x in xs:
            u = [n for n, o in users.get(name, []) if o == x and n in stored]
            if not u:
                break
            nv.append(u[0])
        if len(nv) != 3:
            continue
        ns, ms_ = [], []
        for x in xs:
            pr = products(x)
            if not pr or len(pr) != 3:
                break
            nn, mm = [], []
            for a, b in pr:
                if from_normal(a):
                    nn.append(a), mm.append(b)
                elif from_normal(b):
                    nn.append(b), mm.append(a)
            if len(nn) != 3:
                break
            ns.append(nn)
            ms_.append(mm)
        if len(ms_) != 3 or any(n != ns[0] for n in ns):
            continue
        # LocalToWorld's column for output c: M_0c = B_0 . L_c (an fmad chain over k); the B side is the operand the
        # three outputs share at each k
        chains = [products(ms_[c][0]) for c in range(3)]
        if any(not ch or len(ch) < 3 for ch in chains):
            continue
        cols = [[], [], []]
        ok = True
        for k in range(3):
            terms = [set(ch[k]) for ch in chains]
            shared = terms[0] & terms[1] & terms[2]
            if len(shared) != 1:
                ok = False
                break
            for c in range(3):
                rest = [o for o in chains[c][k] if o not in shared]
                if len(rest) != 1:
                    ok = False
                    break
                cols[c].append(rest[0])
        if not ok:
            continue
        first = min(min(stored[v]) for v in nv)
        if any(defs[v][0] >= first for col in cols for v in col):
            continue
        e = Emit('cn_')
        # dual quaternion skinning (patch_capsules): the normal from the slot's DQS matrix, n . N, into the world with
        # LocalToWorld's columns, in place of the game's while the slot has it on
        def load_of(v):  # the NORMAL input read behind n * 2 - 1
            d = defs.get(v, (0, ''))[1]
            m = re.match(r'fadd fast float (%[\w.]+), -1\.000000e\+00$', d)
            if m:
                m2 = re.match(r'fmul fast float (%[\w.]+), 2\.000000e\+00$', defs.get(m.group(1), (0, ''))[1])
                return m2.group(1) if m2 else None
            m = re.match(r'call float @dx\.op\.tertiary\.f32\(i32 46, float (%[\w.]+), float 2\.000000e\+00, ', d)
            return m.group(1) if m else None

        nl = [None] * 3
        for v in ns[0]:
            lv = load_of(v)
            cm = lv and re.search(r', i8 (\d),', defs.get(lv, (0, ''))[1])
            if cm and int(cm.group(1)) < 3:
                nl[int(cm.group(1))] = v
        nvx = list(nv)
        if '%tw3se_p0Nq' in text and None not in nl and defs.get('%tw3se_p0Nq', (1 << 30,))[0] < first:
            Nm = [[f'%tw3se_p0N{i * 3 + j}' for j in range(3)] for i in range(3)]
            nmod = []
            for j in range(3):
                v = e(f'fmul fast float {nl[0]}, {Nm[0][j]}')
                v = e(f'call float @dx.op.tertiary.f32(i32 46, float {nl[1]}, float {Nm[1][j]}, float {v})')
                nmod.append(e(f'call float @dx.op.tertiary.f32(i32 46, float {nl[2]}, float {Nm[2][j]}, float {v})'))
            nw = []
            for c in range(3):
                v = e(f'fmul fast float {cols[c][0]}, {nmod[0]}')
                v = e(f'call float @dx.op.tertiary.f32(i32 46, float {cols[c][1]}, float {nmod[1]}, float {v})')
                nw.append(e(f'call float @dx.op.tertiary.f32(i32 46, float {cols[c][2]}, float {nmod[2]}, float {v})'))
            nd = e(f'call float @dx.op.dot3.f32(i32 55, float {nw[0]}, float {nw[1]}, float {nw[2]}, float {nw[0]}, '
                   f'float {nw[1]}, float {nw[2]})')
            nr = e(f'call float @dx.op.unary.f32(i32 25, float {e(f"call float @dx.op.binary.f32(i32 35, float {nd}, float {f32hex(1e-12)})")})')
            nvx = [e(f'select i1 %tw3se_p0Nq, float {e(f"fmul fast float {nw[c]}, {nr}")}, float {nv[c]}')
                   for c in range(3)]
        dpush = ['%tw3se_p0dx', '%tw3se_p0dy', '%tw3se_p0dz']
        dw = []
        for c in range(3):
            v = e(f'fmul fast float {cols[c][0]}, {dpush[0]}')
            v = e(f'call float @dx.op.tertiary.f32(i32 46, float {cols[c][1]}, float {dpush[1]}, float {v})')
            dw.append(e(f'call float @dx.op.tertiary.f32(i32 46, float {cols[c][2]}, float {dpush[2]}, float {v})'))
        dd = e(f'call float @dx.op.dot3.f32(i32 55, float {dw[0]}, float {dw[1]}, float {dw[2]}, float {dw[0]}, '
               f'float {dw[1]}, float {dw[2]})')
        ln = e(f'call float @dx.op.unary.f32(i32 24, float {dd})')  # sqrt
        w = e(f'fmul fast float {ln}, {f32hex(1.0 / CAP_NORMAL_FULL)}')
        w = e(f'call float @dx.op.unary.f32(i32 7, float {w})')  # saturate
        dm = e(f'call float @dx.op.binary.f32(i32 35, float {dd}, float {f32hex(1e-12)})')
        rs = e(f'call float @dx.op.unary.f32(i32 25, float {dm})')
        sd = e(f'call float @dx.op.dot3.f32(i32 55, float {dw[0]}, float {dw[1]}, float {dw[2]}, float {nvx[0]}, '
               f'float {nvx[1]}, float {nvx[2]})')
        neg = e(f'fcmp fast olt float {sd}, {f32hex(0.0)}')
        sg = e(f'select i1 {neg}, float {f32hex(-1.0)}, float {f32hex(1.0)}')
        rs = e(f'fmul fast float {rs}, {sg}')
        t = []
        for c in range(3):
            tc = e(f'fmul fast float {dw[c]}, {rs}')
            df = e(f'fsub fast float {tc}, {nvx[c]}')
            t.append(e(f'call float @dx.op.tertiary.f32(i32 46, float {df}, float {w}, float {nvx[c]})'))
        tt = e(f'call float @dx.op.dot3.f32(i32 55, float {t[0]}, float {t[1]}, float {t[2]}, float {t[0]}, '
               f'float {t[1]}, float {t[2]})')
        tr = e(f'call float @dx.op.unary.f32(i32 25, float {tt})')
        new = [e(f'fmul fast float {t[c]}, {tr}') for c in range(3)]
        out = []
        for i, line in enumerate(lines):
            if i == first:
                out += e.lines
            for c in range(3):
                if i in stored[nv[c]]:
                    line = line.replace(f'float {nv[c]})', f'float {new[c]})', 1)
            out.append(line)
        ensure_declare(out, 'declare float @dx.op.dot3.f32(i32, float, float, float, float, float, float) #0')
        return '\n'.join(out) + '\n'
    return text


# Ray tracing: the game skins each figure once more in a compute shader for the ray tracing geometry
# (REDkit include_computeRTData.fx + vertexFactoryMeshSkinned.fx, RS_RAY_TRACING_DATA_STREAM: LocalToWorld is the
# identity there, the position written is the skinned one in model space). A thread is a vertex of one chunk:
# stream 0 at t6 + VSC_Custom_6.x (a byte offset), the vertex count in VSC_Custom_3.w. No draw, no slot set by TW3SE:
# the shader finds its chunk in a list in the table (offset, vertex count, slot, weights) and pushes as the vertex
# shaders do (this frame's capsules).
RT_REC_ROW0 = TABLE_ROWS        # after the capsules
RT_RECS = 64                    # chunks in the list (skinhook.cpp kRecs); row 0's z is how many
CAPTURE_REGISTER = 15           # u15, space7: the capture of the skinned positions (row 0: x slot, y base, w room)
RT_TABLE_ROWS = TABLE_ROWS + RT_RECS


def find_rt(lines):
    """In a compute shader: thread id, chunk offset, vertex count and the store of the position (its line and the
    three floats), or None."""
    code = [l.split(';', 1)[0] for l in lines]
    tid = next((m.group(1) for m in (re.match(r'\s*(%[\w.]+) = call i32 @dx\.op\.threadId\.i32\(i32 93, i32 0\)', c)
                                     for c in code) if m), None)
    if not tid:
        return None
    t = re.escape(tid)
    shl = next((m.group(1) for m in (re.match(rf'\s*(%[\w.]+) = shl i32 {t}, 4$', c.rstrip()) for c in code) if m), None)
    cnt = next((m.group(1) for m in (re.search(rf'icmp ult i32 {t}, (%[\w.]+)', c) for c in code) if m), None)
    if not shl or not cnt:
        return None
    sh = re.escape(shl)
    off = None
    for c in code:
        m = re.match(rf'\s*%[\w.]+ = add i32 (%[\w.]+), {sh}$', c.rstrip()) or \
            re.match(rf'\s*%[\w.]+ = add i32 {sh}, (%[\w.]+)$', c.rstrip())
        if m:
            off = m.group(1)
            break
    muls = {m.group(1) for m in (re.match(rf'\s*(%[\w.]+) = (?:mul|shl) i32 {t}, \d+$', c.rstrip()) for c in code)
            if m}  # (the output's stride: 48 bytes a vertex, or 64)
    adds = {}
    for c in code:
        m = re.match(r'\s*(%[\w.]+) = add i32 (%[\w.]+), (%[\w.]+)$', c.rstrip())
        if m and (m.group(2) in muls or m.group(3) in muls):
            adds[m.group(1)] = True
    defs = {m.group(1): m.group(2) for m in (DEF.match(c.rstrip()) for c in code) if m}
    for i, c in enumerate(code):
        m = re.search(r'rawBufferStore\.i32\(i32 140, %dx\.types\.Handle %[\w.]+, i32 (%[\w.]+), i32 undef, '
                      r'i32 (%[\w.]+), i32 (%[\w.]+), i32 (%[\w.]+), i32 undef, i8 7', c)
        if not m or m.group(1) not in adds:
            continue
        fl = []
        for v in m.group(2, 3, 4):
            b = re.match(r'bitcast float (%[\w.]+) to i32$', defs.get(v, ''))
            if not b:
                return None
            fl.append(b.group(1))
        if not off:
            return None
        return tid, off, cnt, i, fl
    return None


def add_srv_raw(lines, register, space=CB_SPACE):
    """Declares a ByteAddressBuffer at t<register>, space7; returns its range id."""
    ids = [int(m.group(1)) for m in (re.match(r'^!(\d+) = ', l) for l in lines) if m]
    nxt = max(ids) + 1
    res_line = next(i for i, l in enumerate(lines) if l.startswith('!dx.resources = '))
    tup = re.search(r'!\{!(\d+)\}', lines[res_line]).group(1)
    ti = next(i for i, l in enumerate(lines) if l.startswith(f'!{tup} = !{{'))
    elems = [e.strip() for e in lines[ti].split('= !{', 1)[1].rstrip('}').split(',')]
    record = nxt
    if elems[0] == 'null':
        rid, lst = 0, nxt + 1
        elems[0] = f'!{lst}'
        lines[ti] = f'!{tup} = !{{' + ', '.join(elems) + '}'
        lines.append(f'!{lst} = !{{!{record}}}')
    else:
        li = next(i for i, l in enumerate(lines) if l.startswith(f'{elems[0]} = !{{'))
        inner = lines[li].split('= !{', 1)[1].rstrip('}')
        rid = len([x for x in inner.split(',') if x.strip()])
        lines[li] = lines[li].rstrip('}') + f', !{record}}}'
    lines.append(f'!{record} = !{{i32 {rid}, %struct.ByteAddressBuffer* undef, !"", i32 {space}, i32 {register}, '
                 'i32 1, i32 11, i32 0, null}')
    if not any(l.startswith('%struct.ByteAddressBuffer = ') for l in lines):
        di = next(i for i, l in enumerate(lines) if l.startswith('define void @'))
        lines.insert(di, '%struct.ByteAddressBuffer = type { i32 }')
    return rid


def add_shader_flag(lines, flag):
    """ORs a flag into the entry point's shader flags (tag 0), adding the tag when there is none."""
    ep = next(l for l in lines if l.startswith('!dx.entryPoints = '))
    n = re.search(r'!\{!(\d+)\}', ep).group(1)
    ei = next(i for i, l in enumerate(lines) if l.startswith(f'!{n} = !{{'))
    last = lines[ei].rstrip('}').rsplit(',', 1)[1].strip()
    if last == 'null':
        ids = [int(m.group(1)) for m in (re.match(r'^!(\d+) = ', l) for l in lines) if m]
        new = max(ids) + 1
        lines[ei] = lines[ei].rstrip('}').rsplit(',', 1)[0] + f', !{new}}}'
        lines.append(f'!{new} = !{{i32 0, i64 {flag}}}')
        return
    pi = next(i for i, l in enumerate(lines) if l.startswith(f'{last} = !{{'))
    m = re.search(r'i32 0, i64 (\d+)', lines[pi])
    if m:
        lines[pi] = lines[pi][:m.start(1)] + str(int(m.group(1)) | flag) + lines[pi][m.end(1):]
    else:
        lines[pi] = lines[pi].replace('= !{', f'= !{{i32 0, i64 {flag}, ', 1)


def add_uav_raw(lines, register, space=CB_SPACE):
    """Declares a RWByteAddressBuffer at u<register>, space7; returns its range id."""
    ids = [int(m.group(1)) for m in (re.match(r'^!(\d+) = ', l) for l in lines) if m]
    nxt = max(ids) + 1
    res_line = next(i for i, l in enumerate(lines) if l.startswith('!dx.resources = '))
    tup = re.search(r'!\{!(\d+)\}', lines[res_line]).group(1)
    ti = next(i for i, l in enumerate(lines) if l.startswith(f'!{tup} = !{{'))
    elems = [e.strip() for e in lines[ti].split('= !{', 1)[1].rstrip('}').split(',')]
    record = nxt
    if elems[1] == 'null':
        rid, lst = 0, nxt + 1
        elems[1] = f'!{lst}'
        lines[ti] = f'!{tup} = !{{' + ', '.join(elems) + '}'
        lines.append(f'!{lst} = !{{!{record}}}')
    else:
        li = next(i for i, l in enumerate(lines) if l.startswith(f'{elems[1]} = !{{'))
        inner = lines[li].split('= !{', 1)[1].rstrip('}')
        rid = len([x for x in inner.split(',') if x.strip()])
        lines[li] = lines[li].rstrip('}') + f', !{record}}}'
    lines.append(f'!{record} = !{{i32 {rid}, %struct.RWByteAddressBuffer* undef, !"", i32 {space}, i32 {register}, '
                 'i32 1, i32 11, i1 false, i1 false, i1 false, null}')
    if not any(l.startswith('%struct.RWByteAddressBuffer = ') for l in lines):
        di = next(i for i, l in enumerate(lines) if l.startswith('define void @'))
        lines.insert(di, '%struct.RWByteAddressBuffer = type { i32 }')
    return rid


def add_binding_handles(e_lines, rid_tab, rid_w, capture=False):
    """SM 6.6 handles (createHandleFromBinding + annotateHandle) of b13 and t14, space7 (and u15 with capture)."""
    cap = [
        f'  %tw3se_uh0 = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind {{ i32 '
        f'{CAPTURE_REGISTER}, i32 {CAPTURE_REGISTER}, i32 {CB_SPACE}, i8 1 }}, i32 {CAPTURE_REGISTER}, i1 false)',
        '  %tw3se_uh = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %tw3se_uh0, '
        '%dx.types.ResourceProperties { i32 4107, i32 0 })',
    ] if capture else []
    return cap + [
        f'  %tw3se_cb0 = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind {{ i32 '
        f'{CB_REGISTER}, i32 {CB_REGISTER}, i32 {CB_SPACE}, i8 2 }}, i32 {CB_REGISTER}, i1 false)',
        f'  %tw3se_cb = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %tw3se_cb0, '
        f'%dx.types.ResourceProperties {{ i32 13, i32 {16 * RT_TABLE_ROWS} }})',
        f'  %tw3se_wh0 = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind {{ i32 '
        f'{WEIGHT_REGISTER}, i32 {WEIGHT_REGISTER}, i32 {CB_SPACE}, i8 0 }}, i32 {WEIGHT_REGISTER}, i1 false)',
        '  %tw3se_wh = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %tw3se_wh0, '
        '%dx.types.ResourceProperties { i32 12, i32 4 })',
    ]


def patch_rt(text):
    """The ray tracing skinning compute shader: its chunk looked up in the table's list, then the push."""
    body, lines = parse(text)
    found = find_rt(lines)
    if not found:
        raise SystemExit('not a ray tracing skinning shader (no thread id, chunk offset or position store found)')
    tid, off, cntv, store_i, (px, py, pz) = found
    where = {name: idx for idx, name, _ in body if name}
    last = max(where[px], where[py], where[pz])
    for v in (px, py, pz):
        for i in range(where[v] + 1, last + 1):
            if re.search(re.escape(v) + r'(?![\w.])', lines[i].split(';', 1)[0].split('=', 1)[-1]):
                raise SystemExit(f'{v} is used before the position is complete (line {i + 1}): not patched')
    out, rename, pred_map, block = [], {}, {}, '%0'
    for i, line in enumerate(lines):
        m = LABEL.match(line) or NAMED_LABEL.match(line)
        if m and not line.startswith('define') and i > body[0][0] - 1:
            block = '%' + m.group(1)
        code, sep, comment = line.partition(';')
        if rename:
            dm = DEF.match(code.rstrip())
            own = dm.group(1) if dm else None
            code = OPND.sub(lambda mo: rename.get(mo.group(0), mo.group(0)) if mo.group(0) != own else mo.group(0), code)
        if ' phi ' in code and pred_map:
            def pred(mo):
                lab = mo.group(2)
                while lab in pred_map:
                    lab = pred_map[lab]
                return mo.group(1) + lab + mo.group(3)
            code = PHI_PRED.sub(pred, code)
        out.append(code + sep + comment if sep else code)
        if i != last:
            continue
        # the list: (offset, vertex count, slot, weights) per row; at least one round (rows past the count are zero)
        n0 = '%tw3se_rt_n0'
        out += [
            '  %tw3se_rt_h = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %tw3se_cb, '
            'i32 0)',
            f'  {n0} = extractvalue %dx.types.CBufRet.i32 %tw3se_rt_h, 2',
            f'  %tw3se_rt_big = icmp ugt i32 {n0}, {RT_RECS}',
            f'  %tw3se_rt_n = select i1 %tw3se_rt_big, i32 {RT_RECS}, i32 {n0}',
            '  br label %tw3se_rt_loop',
            '',
            'tw3se_rt_loop:',
            f'  %tw3se_rt_i = phi i32 [ 0, {block} ], [ %tw3se_rt_i1, %tw3se_rt_loop ]',
            f'  %tw3se_rt_s = phi i32 [ 0, {block} ], [ %tw3se_rt_s1, %tw3se_rt_loop ]',
            f'  %tw3se_rt_w = phi i32 [ -1, {block} ], [ %tw3se_rt_w1, %tw3se_rt_loop ]',
            f'  %tw3se_rt_row = add i32 %tw3se_rt_i, {RT_REC_ROW0}',
            '  %tw3se_rt_r = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %tw3se_cb, '
            'i32 %tw3se_rt_row)',
            '  %tw3se_rt_ro = extractvalue %dx.types.CBufRet.i32 %tw3se_rt_r, 0',
            '  %tw3se_rt_rc = extractvalue %dx.types.CBufRet.i32 %tw3se_rt_r, 1',
            '  %tw3se_rt_rs = extractvalue %dx.types.CBufRet.i32 %tw3se_rt_r, 2',
            '  %tw3se_rt_rw = extractvalue %dx.types.CBufRet.i32 %tw3se_rt_r, 3',
            f'  %tw3se_rt_m1 = icmp eq i32 %tw3se_rt_ro, {off}',
            f'  %tw3se_rt_m2 = icmp eq i32 %tw3se_rt_rc, {cntv}',
            '  %tw3se_rt_m = and i1 %tw3se_rt_m1, %tw3se_rt_m2',
            '  %tw3se_rt_s1 = select i1 %tw3se_rt_m, i32 %tw3se_rt_rs, i32 %tw3se_rt_s',
            '  %tw3se_rt_w1 = select i1 %tw3se_rt_m, i32 %tw3se_rt_rw, i32 %tw3se_rt_w',
            '  %tw3se_rt_i1 = add i32 %tw3se_rt_i, 1',
            '  %tw3se_rt_more = icmp ult i32 %tw3se_rt_i1, %tw3se_rt_n',
            '  br i1 %tw3se_rt_more, label %tw3se_rt_loop, label %tw3se_rt_after',
            '',
            'tw3se_rt_after:',
            f'  %tw3se_rt_sb = icmp ugt i32 %tw3se_rt_s1, {SLOTS}',
            '  %tw3se_slot = select i1 %tw3se_rt_sb, i32 0, i32 %tw3se_rt_s1',
            '  %tw3se_wbase = add i32 %tw3se_rt_w1, 0',
            f'  %tw3se_vid = add i32 {tid}, 0',
            '  %tw3se_cnt = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %tw3se_cb, '
            'i32 %tw3se_slot)',
            f'  %tw3se_capb0 = mul i32 %tw3se_slot, {CAPS * CAP_ROWS * 2}',
            f'  %tw3se_capbase = add i32 %tw3se_capb0, {CAP_ROW0}',
            '  %tw3se_cntw = extractvalue %dx.types.CBufRet.i32 %tw3se_cnt, 3',
        ]
        e = Emit('rt_')
        # dual quaternion skinning, as in the vertex shaders (the slot's row w, bit 17)
        # The stored position is the skinned one S, or a blend B + t * (S - B) with a position B of another source:
        # the DQS position goes in for S (final + t * (DQS - S)).
        op = {name: rhs for _, name, rhs in body if name}

        def lerp_parts(v):
            m = re.match(r'fadd (?:fast )?float (%[\w.]+), (%[\w.]+)$', op.get(v, ''))
            for x, b in ((m.group(1), m.group(2)), (m.group(2), m.group(1))) if m else ():
                mm = re.match(r'fmul (?:fast )?float (%[\w.]+), (%[\w.]+)$', op.get(x, ''))
                for t, d in ((mm.group(1), mm.group(2)), (mm.group(2), mm.group(1))) if mm else ():
                    md = re.match(r'fsub (?:fast )?float (%[\w.]+), (%[\w.]+)$', op.get(d, ''))
                    if md and md.group(2) == b:
                        return md.group(1), t
            return v, None

        parts = [lerp_parts(v) for v in (px, py, pz)]
        skin = [s for s, _ in parts]
        ts = {t for _, t in parts}
        start, cx, cy, cz = '%tw3se_rt_do', px, py, pz
        dqi = dqs_inputs(body, skin) if len(ts) == 1 else None
        if dqi:
            dqi['lbs'] = skin
            tmix = ts.pop()
            q0 = e('and i32 %tw3se_cntw, 131072')
            qon = e(f'icmp ne i32 {q0}, 0')
            e.lines += [f'  br i1 {qon}, label %tw3se_rt_qdo, label %tw3se_rt_qj', '', 'tw3se_rt_qdo:']
            qv = dqs_code(e, dqi)
            fin = []
            for c, (v, s) in enumerate(zip((px, py, pz), skin)):
                d = e(f'fsub float {qv[c]}, {s}')
                if tmix:
                    d = e(f'fmul float {d}, {tmix}')
                fin.append(e(f'fadd float {v}, {d}'))
            e.lines += ['  br label %tw3se_rt_qj', '', 'tw3se_rt_qj:']
            cx, cy, cz = (e(f'phi float [ {nv_}, %tw3se_rt_qdo ], [ {ov}, {start} ]')
                          for nv_, ov in zip(fin, (px, py, pz)))
            start = '%tw3se_rt_qj'
        else:
            print('  dqs: the ray tracing skinning not understood, left linear', file=sys.stderr)
        nx, ny, nz, last = capsule_code(e, cx, cy, cz, 0, start)
        out += ['  %tw3se_rt_go = icmp ne i32 %tw3se_slot, 0',
                '  br i1 %tw3se_rt_go, label %tw3se_rt_do, label %tw3se_rt_join', '', 'tw3se_rt_do:']
        out += e.lines
        out += ['  br label %tw3se_rt_join', '', 'tw3se_rt_join:']
        for c, v, nv in zip('xyz', (px, py, pz), (nx, ny, nz)):
            out.append(f'  %tw3se_rt_{c} = phi float [ {nv}, {last} ], [ {v}, %tw3se_rt_after ]')
            rename[v] = f'%tw3se_rt_{c}'
        # the capture: the finished position of each vertex of the capture slot's mesh into u15
        out += [
            '  %tw3se_cap_r = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %tw3se_cb, '
            'i32 0)',
            '  %tw3se_cap_s = extractvalue %dx.types.CBufRet.i32 %tw3se_cap_r, 0',
            '  %tw3se_cap_b = extractvalue %dx.types.CBufRet.i32 %tw3se_cap_r, 1',
            '  %tw3se_cap_l = extractvalue %dx.types.CBufRet.i32 %tw3se_cap_r, 3',
            '  %tw3se_cap_m1 = icmp eq i32 %tw3se_cap_s, %tw3se_slot',
            '  %tw3se_cap_m2 = icmp ne i32 %tw3se_slot, 0',
            f'  %tw3se_cap_m3 = icmp uge i32 {off}, %tw3se_cap_b',
            '  %tw3se_cap_m12 = and i1 %tw3se_cap_m1, %tw3se_cap_m2',
            '  %tw3se_cap_m = and i1 %tw3se_cap_m12, %tw3se_cap_m3',
            f'  %tw3se_cap_d = sub i32 {off}, %tw3se_cap_b',
            '  %tw3se_cap_q = lshr i32 %tw3se_cap_d, 4',
            f'  %tw3se_cap_i = add i32 %tw3se_cap_q, {tid}',
            '  %tw3se_cap_in = icmp ult i32 %tw3se_cap_i, %tw3se_cap_l',
            '  %tw3se_cap_go = and i1 %tw3se_cap_m, %tw3se_cap_in',
            '  br i1 %tw3se_cap_go, label %tw3se_cap_do, label %tw3se_cap_join',
            '',
            'tw3se_cap_do:',
            '  %tw3se_cap_at = shl i32 %tw3se_cap_i, 4',
            '  call void @dx.op.rawBufferStore.f32(i32 140, %dx.types.Handle %tw3se_uh, i32 %tw3se_cap_at, i32 undef, '
            'float %tw3se_rt_x, float %tw3se_rt_y, float %tw3se_rt_z, float 1.000000e+00, i8 15, i32 4)',
            '  br label %tw3se_cap_join',
            '',
            'tw3se_cap_join:',
        ]
        pred_map[block] = '%tw3se_cap_join'
        block = '%tw3se_cap_join'
    rid_tab = add_cbv(out, 16 * RT_TABLE_ROWS)
    rid_w = add_srv_float(out, WEIGHT_REGISTER)
    add_uav_raw(out, CAPTURE_REGISTER)
    sd = next((l for l in out if l.startswith('declare void @dx.op.rawBufferStore.i32(')), None)
    attr = sd.rsplit(' ', 1)[1] if sd else '#1'
    ensure_declare(out, 'declare void @dx.op.rawBufferStore.f32(i32, %dx.types.Handle, i32, i32, float, float, float, '
                        'float, i8, i32) ' + attr)
    for decl in ('declare %dx.types.ResRet.f32 @dx.op.rawBufferLoad.f32(i32, %dx.types.Handle, i32, i32, i8, i32) #1',
                 'declare %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32, %dx.types.Handle, i32) #2',
                 'declare %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32, %dx.types.Handle, i32) #2',
                 'declare float @dx.op.binary.f32(i32, float, float) #0',
                 'declare float @dx.op.unary.f32(i32, float) #0',
                 'declare float @dx.op.tertiary.f32(i32, float, float, float) #0',
                 'declare %dx.types.Handle @dx.op.createHandleFromBinding(i32, %dx.types.ResBind, i32, i1) #0',
                 'declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, %dx.types.ResourceProperties) #0'):
        ensure_declare(out, decl)
    for t, d in (('%dx.types.ResRet.f32', '{ float, float, float, float, i32 }'),
                 ('%dx.types.CBufRet.i32', '{ i32, i32, i32, i32 }'),
                 ('%dx.types.CBufRet.f32', '{ float, float, float, float }')):
        if not any(l.startswith(t + ' = ') for l in out):
            ti = next(i for i, l in enumerate(out) if l.startswith('%dx.types.Handle = '))
            out.insert(ti + 1, f'{t} = type {d}')
    di = next(i for i, l in enumerate(out) if l.startswith('define void @'))
    out[di + 1:di + 1] = add_binding_handles(out, rid_tab, rid_w, capture=True)
    return '\n'.join(out) + '\n', [(px, py, pz)]


OVERLAY_REGISTER = 16  # t16, space7: the skin overlays (RGBA8 texels); the slot's row of the table: z first, w size
OVERLAY_MAX = 512  # texels a side, a slot; the material planes follow the colour planes of all slots
BUMP = 16.0        # B = 1 stands that many texels high: the normal tilts by the slope times it
MATTE = 1.0        # the roughness a fully matte texel goes to


def patch_skin_ps(text):
    """The overlay mixed into the albedo of a material pixel shader (see the patch's header); raises SystemExit when
    the shader has no such target."""
    body, lines = parse(text)
    defs = {name: (idx, rhs) for idx, name, rhs in body if name}
    stores = {}
    for idx, name, rhs in body:
        m = re.match(r'call void @dx\.op\.storeOutput\.f32\(i32 5, i32 (\d+), i32 0, i8 (\d), float (%[\w.]+)\)', rhs)
        if m:
            stores[(int(m.group(1)), int(m.group(2)))] = m.group(3)
    users = {}
    for idx, name, rhs in body:
        for o in OPND.findall(rhs):
            users.setdefault(o, []).append(rhs)
    target = None
    for row in sorted({r for r, _ in stores}):
        vals = [stores.get((row, c)) for c in range(3)]
        if None in vals:
            continue
        srcs = []
        for v in vals:
            d = defs.get(v)
            m = re.match(r'call float @dx\.op\.unary\.f32\(i32 7, float (%[\w.]+)\)$', d[1]) if d else None
            if not m or not any('@dx.op.unary.f32(i32 23, float ' + v + ')' in u for u in users.get(v, [])):
                srcs = None
                break
            srcs.append((v, m.group(1), d[0]))
        if srcs:
            target = srcs
            break
    if not target:
        raise SystemExit('no albedo target (three saturates going into a log)')
    uv = None
    for idx, name, rhs in body:
        m = re.match(r'call %dx\.types\.ResRet\.f32 @dx\.op\.sample\.f32\(i32 60, %dx\.types\.Handle %[\w.]+, '
                     r'%dx\.types\.Handle %[\w.]+, float (%[\w.]+), float (%[\w.]+),', rhs)
        if not m:
            continue
        ok = all(re.match(r'call float @dx\.op\.loadInput\.f32\(i32 4, i32 \d+, i32 0, i8 [01], i32 undef\)',
                          defs.get(v, (0, ''))[1]) for v in m.group(1, 2))
        if ok:
            uv = m.group(1, 2)
            break
    if not uv:
        raise SystemExit('no material UV (a sample from inputs)')
    at = min(t[2] for t in target)  # before the first of the three saturates
    for v, src, _ in target:
        if defs[src][0] >= at:
            raise SystemExit('the colour is made after its saturate')
    # The material plane (R wet, G matte, B height) goes where the shader has the pieces: the game's own wetness
    # (rain: a vertex input times 0.65, it drives the albedo and the roughness), the roughness (w of the normal
    # target) and the normal (n * 0.5 + 0.5 of a sum t.x * T + t.y * B + t.z * N). A piece the shader lacks is left out.
    wet = None
    for idx, name, rhs in body:
        m = name and re.match(r'fmul fast float (%[\w.]+), 0x3FE4CCCCC0000000$', rhs)
        if m and defs.get(m.group(1), (0, ''))[1].startswith('call float @dx.op.loadInput.f32('):
            wet = (idx, name)
            break
    normal = None
    for row in sorted({r for r, _ in stores}):
        vs = []
        for c in range(3):
            d = defs.get(stores.get((row, c)), (0, ''))[1]
            m = re.match(r'fadd fast float (%[\w.]+), 5\.000000e-01$', d)
            m = m and re.match(r'fmul fast float (%[\w.]+), 5\.000000e-01$', defs.get(m.group(1), (0, ''))[1])
            if not m:
                break
            vs.append(m.group(1))
        if len(vs) < 3 or (row, 3) not in stores:
            continue
        leaves = []  # per component: [(leaf name, tangent-space factor)] in sum order
        for v in vs:
            lv, todo = [], [v]
            while todo and lv is not None:
                n = todo.pop(0)
                d = defs.get(n, (0, ''))[1]
                ma = re.match(r'fadd fast float (%[\w.]+), (%[\w.]+)$', d)
                mm = re.match(r'fmul fast float (%[\w.]+), (%[\w.]+)$', d)
                if ma:
                    todo = list(ma.group(1, 2)) + todo
                elif mm:
                    lv.append((n, mm.group(1)))
                else:
                    lv = None
            leaves.append(lv)
        if all(lv and len(lv) == 3 for lv in leaves) and all([f for _, f in lv] == [f for _, f in leaves[0]]
                                                                  for lv in leaves):
            ts = [f for _, f in leaves[0]]
            uses = sorted(defs[n][0] for lv in leaves for n, _ in lv)
            if all(t in defs and defs[t][0] < uses[0] for t in ts):
                normal = (row, ts, uses)
            break
    first = min([at] + ([wet[0]] if wet else []) + ([normal[2][0]] if normal else []))
    if any(defs[v][0] >= first for v in uv):
        raise SystemExit('the UV is read after the first use')
    e = Emit('ov_')
    cnt = e('call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %tw3se_osh, i32 0)')
    slot0 = e(f'extractvalue %dx.types.CBufRet.i32 {cnt}, 0')
    big = e(f'icmp ugt i32 {slot0}, {SLOTS}')
    slot = e(f'select i1 {big}, i32 0, i32 {slot0}')
    row = e(f'call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %tw3se_ocb, i32 {slot})')
    base = e(f'extractvalue %dx.types.CBufRet.i32 {row}, 2')
    sizew = e(f'extractvalue %dx.types.CBufRet.i32 {row}, 3')
    size = e(f'and i32 {sizew}, 65535')  # bit 16: the material plane is there (a TW3SE that has it sets it)
    mbit = e(f'lshr i32 {sizew}, 16')
    mbit = e(f'and i32 {mbit}, 1')
    moff = e(f'mul i32 {mbit}, {OVERLAY_MAX * OVERLAY_MAX * SLOTS}')
    has = e(f'icmp ne i32 {slot}, 0')
    sized = e(f'icmp ne i32 {size}, 0')
    on = e(f'and i1 {has}, {sized}')
    do, join = 'tw3se_ov_do', 'tw3se_ov_join'
    e.lines += [f'  br i1 {on}, label %{do}, label %{join}', '', f'{do}:']
    zero, one = f32hex(0.0), f32hex(1.0)
    fs = e(f'uitofp i32 {size} to float')
    fm = e(f'fadd fast float {fs}, {f32hex(-1.0)}')
    mf = e(f'uitofp i32 {mbit} to float')
    ix, fx = [], []
    for k in range(2):
        u = e(f'call float @dx.op.unary.f32(i32 22, float {uv[k]})')  # frac: the UV wraps
        u = e(f'fmul fast float {u}, {fm}')
        f0 = e(f'call float @dx.op.unary.f32(i32 27, float {u})')  # floor
        fr = e(f'fsub fast float {u}, {f0}')
        f1 = e(f'fadd fast float {f0}, {one}')
        f1 = e(f'call float @dx.op.binary.f32(i32 36, float {f1}, float {fm})')  # min
        ix.append((e(f'fptoui float {f0} to i32'), e(f'fptoui float {f1} to i32')))
        fx.append(fr)
    texel = {}
    for plane, off in ((0, None), (1, moff)):  # without the bit the material reads the colour again, times 0
        for (yi, yv) in ((0, ix[1][0]), (1, ix[1][1])):
            rowo = e(f'mul i32 {yv}, {size}')
            for (xi, xv) in ((0, ix[0][0]), (1, ix[0][1])):
                t = e(f'add i32 {rowo}, {xv}')
                t = e(f'add i32 {t}, {base}')
                if off:
                    t = e(f'add i32 {t}, {off}')
                b = e(f'shl i32 {t}, 2')
                ld = e(f'call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %tw3se_ovh, '
                       f'i32 {b}, i32 undef, i8 1, i32 4)')
                w = e(f'extractvalue %dx.types.ResRet.i32 {ld}, 0')
                ch = []
                for c in range(4):
                    sh = e(f'lshr i32 {w}, {8 * c}') if c else w
                    bt = e(f'and i32 {sh}, 255')
                    fl = e(f'uitofp i32 {bt} to float')
                    ch.append(e(f'fmul fast float {fl}, {f32hex(1.0 / 255.0)}'))
                texel[(plane, xi, yi)] = ch
    col = []
    for p, c in ((0, 0), (0, 1), (0, 2), (0, 3), (1, 0), (1, 1)):
        a0 = e(f'fsub fast float {texel[(p, 1, 0)][c]}, {texel[(p, 0, 0)][c]}')
        a0 = e(f'call float @dx.op.tertiary.f32(i32 46, float {a0}, float {fx[0]}, float {texel[(p, 0, 0)][c]})')
        a1 = e(f'fsub fast float {texel[(p, 1, 1)][c]}, {texel[(p, 0, 1)][c]}')
        a1 = e(f'call float @dx.op.tertiary.f32(i32 46, float {a1}, float {fx[0]}, float {texel[(p, 0, 1)][c]})')
        d = e(f'fsub fast float {a1}, {a0}')
        v = e(f'call float @dx.op.tertiary.f32(i32 46, float {d}, float {fx[1]}, float {a0})')
        col.append(e(f'fmul fast float {v}, {mf}') if p else v)
    # the height's slope (B of the material plane, height units a texel): central differences at the four texels
    # around the point, from a 4 x 4 block of heights (the outer row and column clamped at the picture's edge), mixed
    # bilinearly - smooth over the texels
    outer = []
    for k in range(2):
        u = e(f'call float @dx.op.unary.f32(i32 22, float {uv[k]})')
        u = e(f'fmul fast float {u}, {fm}')
        f0 = e(f'call float @dx.op.unary.f32(i32 27, float {u})')
        lo = e(f'fadd fast float {f0}, {f32hex(-1.0)}')
        lo = e(f'call float @dx.op.binary.f32(i32 35, float {lo}, float {zero})')  # max
        hi = e(f'fadd fast float {f0}, {f32hex(2.0)}')
        hi = e(f'call float @dx.op.binary.f32(i32 36, float {hi}, float {fm})')  # min
        outer.append((e(f'fptoui float {lo} to i32'), e(f'fptoui float {hi} to i32')))
    xs = [outer[0][0], ix[0][0], ix[0][1], outer[0][1]]
    ys = [outer[1][0], ix[1][0], ix[1][1], outer[1][1]]
    hgt = {}
    for j in range(4):
        for i in range(4):
            if 1 <= i <= 2 and 1 <= j <= 2:
                hgt[(i, j)] = texel[(1, i - 1, j - 1)][2]
                continue
            if (i in (0, 3)) and (j in (0, 3)):
                continue  # the corners of the block are not needed
            rowo = e(f'mul i32 {ys[j]}, {size}')
            t = e(f'add i32 {rowo}, {xs[i]}')
            t = e(f'add i32 {t}, {base}')
            t = e(f'add i32 {t}, {moff}')
            bb = e(f'shl i32 {t}, 2')
            ld = e(f'call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %tw3se_ovh, '
                   f'i32 {bb}, i32 undef, i8 1, i32 4)')
            w = e(f'extractvalue %dx.types.ResRet.i32 {ld}, 0')
            sh = e(f'lshr i32 {w}, 16')
            bt = e(f'and i32 {sh}, 255')
            fl = e(f'uitofp i32 {bt} to float')
            hgt[(i, j)] = e(f'fmul fast float {fl}, {f32hex(1.0 / 255.0)}')
    half = f32hex(0.5)
    gxs, gys = {}, {}
    for j in (1, 2):
        for i in (1, 2):
            d = e(f'fsub fast float {hgt[(i + 1, j)]}, {hgt[(i - 1, j)]}')
            gxs[(i, j)] = e(f'fmul fast float {d}, {half}')
            d = e(f'fsub fast float {hgt[(i, j + 1)]}, {hgt[(i, j - 1)]}')
            gys[(i, j)] = e(f'fmul fast float {d}, {half}')
    for g in (gxs, gys):
        a0 = e(f'fsub fast float {g[(2, 1)]}, {g[(1, 1)]}')
        a0 = e(f'call float @dx.op.tertiary.f32(i32 46, float {a0}, float {fx[0]}, float {g[(1, 1)]})')
        a1 = e(f'fsub fast float {g[(2, 2)]}, {g[(1, 2)]}')
        a1 = e(f'call float @dx.op.tertiary.f32(i32 46, float {a1}, float {fx[0]}, float {g[(1, 2)]})')
        d = e(f'fsub fast float {a1}, {a0}')
        v = e(f'call float @dx.op.tertiary.f32(i32 46, float {d}, float {fx[1]}, float {a0})')
        col.append(e(f'fmul fast float {v}, {mf}'))
    e.lines += [f'  br label %{join}', '', f'{join}:']
    # after the join: colour r g b a, wet, matte, slope x, slope y (all 0 where the slot has no overlay)
    names = ['c0', 'c1', 'c2', 'ca', 'wet', 'matte', 'gx', 'gy']
    got = {n: f'%tw3se_ov_{n}' for n in names}
    inserts, line_maps, global_maps = {}, {}, []
    mix = Emit('ovm_')
    for k, (v, src, _) in enumerate(target):
        d = mix(f'fsub fast float {got["c" + str(k)]}, {src}')
        global_maps.append((at, src, mix(f'call float @dx.op.tertiary.f32(i32 46, float {d}, float {got["ca"]}, '
                                         f'float {src})')))
    inserts[at] = mix.lines
    if wet:
        we = Emit('ovw_')
        wname = we(f'call float @dx.op.binary.f32(i32 35, float {wet[1]}, float {got["wet"]})')  # max
        inserts.setdefault(wet[0] + 1, []).extend(we.lines)
        global_maps.append((wet[0] + 1, wet[1], wname))
    if normal:
        nrow, ts, uses = normal
        ne = Emit('ovn_')
        k = f32hex(BUMP)
        tx = ne(f'call float @dx.op.tertiary.f32(i32 46, float {got["gx"]}, float {f32hex(-BUMP)}, float {ts[0]})')
        ty = ne(f'call float @dx.op.tertiary.f32(i32 46, float {got["gy"]}, float {k}, float {ts[1]})')
        dd = ne(f'call float @dx.op.dot3.f32(i32 55, float {tx}, float {ty}, float {ts[2]}, float {tx}, float {ty}, '
                f'float {ts[2]})')
        rs = ne(f'call float @dx.op.unary.f32(i32 25, float {dd})')  # rsqrt
        tn = [ne(f'fmul fast float {tx}, {rs}'), ne(f'fmul fast float {ty}, {rs}'), ne(f'fmul fast float {ts[2]}, {rs}')]
        inserts.setdefault(uses[0], []).extend(ne.lines)
        for u in uses:
            line_maps[u] = dict(zip(ts, tn))
        rough = stores[(nrow, 3)]
        si = next(idx for idx, name, rhs in body if rhs.startswith(
            f'call void @dx.op.storeOutput.f32(i32 5, i32 {nrow}, i32 0, i8 3, float {rough})'))
        me = Emit('ovr_')
        d = me(f'fsub fast float {f32hex(MATTE)}, {rough}')
        line_maps[si] = {rough: me(f'call float @dx.op.tertiary.f32(i32 46, float {d}, float {got["matte"]}, '
                                   f'float {rough})')}
        inserts.setdefault(si, []).extend(me.lines)
    out, rename, pred_map, block = [], {}, {}, '%0'
    for i, line in enumerate(lines):
        mm = LABEL.match(line) or NAMED_LABEL.match(line)
        if mm and not line.startswith('define') and i > body[0][0] - 1:
            block = '%' + mm.group(1)
        if i == first:
            out += e.lines
            for n, v in zip(names, col):
                out.append(f'  {got[n]} = phi float [ {v}, %{do} ], [ {zero}, {block} ]')
            pred_map[block] = f'%{join}'
            block = f'%{join}'
        add = inserts.get(i, [])
        if add:
            out += [OPND.sub(lambda mo: rename.get(mo.group(0), mo.group(0)), l) for l in add]
        for start, a, b in global_maps:
            if i == start:
                rename[a] = b
        code_, sep, comment = line.partition(';')
        lm = dict(rename, **line_maps.get(i, {}))
        if lm:
            dm = DEF.match(code_.rstrip())
            own = dm.group(1) if dm else None
            code_ = OPND.sub(lambda mo: lm.get(mo.group(0), mo.group(0)) if mo.group(0) != own else mo.group(0), code_)
        if ' phi ' in code_ and pred_map:
            def pred(mo):
                lab = mo.group(2)
                while lab in pred_map:
                    lab = pred_map[lab]
                return mo.group(1) + lab + mo.group(3)
            code_ = PHI_PRED.sub(pred, code_)
        out.append(code_ + sep + comment if sep else code_)
    add_cbv(out, 16, SLOT_REGISTER, '%tw3se.SkinSlot')
    add_cbv(out, 16 * TABLE_ROWS)
    add_srv_raw(out, OVERLAY_REGISTER)
    for decl in ('declare %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32, %dx.types.Handle, i32, i32, i8, i32) #2',
                 'declare %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32, %dx.types.Handle, i32) #2',
                 'declare float @dx.op.binary.f32(i32, float, float) #0',
                 'declare float @dx.op.unary.f32(i32, float) #0',
                 'declare float @dx.op.tertiary.f32(i32, float, float, float) #0',
                 'declare float @dx.op.dot3.f32(i32, float, float, float, float, float, float) #0',
                 'declare %dx.types.Handle @dx.op.createHandleFromBinding(i32, %dx.types.ResBind, i32, i1) #0',
                 'declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, %dx.types.ResourceProperties) #0'):
        ensure_declare(out, decl)
    for t, d in (('%dx.types.ResRet.i32', '{ i32, i32, i32, i32, i32 }'), ('%dx.types.CBufRet.i32', '{ i32, i32, i32, i32 }')):
        if not any(l.startswith(t + ' = ') for l in out):
            ti = next(i for i, l in enumerate(out) if l.startswith('%dx.types.Handle = '))
            out.insert(ti + 1, f'{t} = type {d}')
    di = next(i for i, l in enumerate(out) if l.startswith('define void @'))
    out[di + 1:di + 1] = [
        f'  %tw3se_osh0 = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind {{ i32 '
        f'{SLOT_REGISTER}, i32 {SLOT_REGISTER}, i32 {CB_SPACE}, i8 2 }}, i32 {SLOT_REGISTER}, i1 false)',
        '  %tw3se_osh = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %tw3se_osh0, '
        '%dx.types.ResourceProperties { i32 13, i32 16 })',
        f'  %tw3se_ocb0 = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind {{ i32 '
        f'{CB_REGISTER}, i32 {CB_REGISTER}, i32 {CB_SPACE}, i8 2 }}, i32 {CB_REGISTER}, i1 false)',
        f'  %tw3se_ocb = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %tw3se_ocb0, '
        f'%dx.types.ResourceProperties {{ i32 13, i32 {16 * TABLE_ROWS} }})',
        f'  %tw3se_ovh0 = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind {{ i32 '
        f'{OVERLAY_REGISTER}, i32 {OVERLAY_REGISTER}, i32 {CB_SPACE}, i8 0 }}, i32 {OVERLAY_REGISTER}, i1 false)',
        '  %tw3se_ovh = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %tw3se_ovh0, '
        '%dx.types.ResourceProperties { i32 11, i32 0 })',
    ]
    return '\n'.join(out) + '\n'


def patch(text, inject):
    if inject == 'rt':
        return patch_rt(text)
    body, lines = parse(text)
    positions = find_positions(body)
    if not positions:
        raise SystemExit('no skinned position found')
    kind, _, arg = inject.partition(':')
    if kind == 'capsules':
        return patch_capsules(text)
    where = {name: idx for idx, name, _ in body if name}
    rename = {}
    inserts = {}
    for p, (x, y, z, w) in enumerate(positions):
        if kind == 'scale':
            k = f32hex(float(arg))
            for c, v in zip('xyz', (x, y, z)):
                nv = f'%tw3se_p{p}{c}'
                # right after the component's own definition: every use of it comes later
                inserts.setdefault(where[v], []).append(f'  {nv} = fmul float {v}, {k}')
                rename[v] = nv
        elif kind in ('offset', 'slot_offset'):
            # the offset (model space, metres) from TW3SE's constant buffer: row 0 (offset), or the row of the slot of
            # the mesh drawn (slot_offset); added to every skinned position (this frame's and the previous one's)
            for c, v in zip('xyz', (x, y, z)):
                nv = f'%tw3se_p{p}{c}'
                inserts.setdefault(where[v], []).append(f'  {nv} = fadd float {v}, %tw3se_o{c}')
                rename[v] = nv
        else:
            raise SystemExit('unknown inject ' + inject)
    out = []
    defined_new = set()
    for i, line in enumerate(lines):
        code, sep, comment = line.partition(';')
        m = DEF.match(code.rstrip())
        own = m.group(1) if m else None
        # every use after the new values exist reads them (not the definitions themselves)
        if defined_new:
            def sub(mo):
                v = mo.group(0)
                return rename[v] if v in rename and v != own and rename[v] in defined_new else v
            code = OPND.sub(sub, code)
        out.append(code + sep + comment if sep else code)
        if i in inserts:
            out.extend(inserts[i])
            for nl in inserts[i]:
                defined_new.add(nl.split('=')[0].strip())
    if kind == 'offset':
        # TW3SE's constant buffer declared, row 0 read at the function's start
        rid = add_cbv(out, 16)
        di = next(i for i, l in enumerate(out) if l.startswith('define void @'))
        out[di + 1:di + 1] = [
            f'  %tw3se_cb = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 {rid}, i32 {CB_REGISTER}, '
            'i1 false)',
            '  %tw3se_cbr = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle '
            '%tw3se_cb, i32 0)',
        ] + [f'  %tw3se_o{c} = extractvalue %dx.types.CBufRet.f32 %tw3se_cbr, {k}' for k, c in enumerate('xyz')]
    if kind == 'slot_offset':
        # b12: the slot (a root constant, uint), b13: one row per slot
        rid_slot = add_cbv(out, 16, SLOT_REGISTER, '%tw3se.SkinSlot')
        rid_tab = add_cbv(out, 16 * (SLOTS + 1))
        ensure_declare(out, 'declare %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32, %dx.types.Handle, i32) #2')
        if not any(l.startswith('%dx.types.CBufRet.i32 = ') for l in out):
            ti = next(i for i, l in enumerate(out) if l.startswith('%dx.types.CBufRet.f32 = '))
            out.insert(ti + 1, '%dx.types.CBufRet.i32 = type { i32, i32, i32, i32 }')
        di = next(i for i, l in enumerate(out) if l.startswith('define void @'))
        out[di + 1:di + 1] = [
            f'  %tw3se_sh = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 {rid_slot}, i32 {SLOT_REGISTER}, '
            'i1 false)',
            f'  %tw3se_cb = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 {rid_tab}, i32 {CB_REGISTER}, '
            'i1 false)',
            '  %tw3se_sr = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %tw3se_sh, '
            'i32 0)',
            '  %tw3se_slot0 = extractvalue %dx.types.CBufRet.i32 %tw3se_sr, 0',
            f'  %tw3se_big = icmp ugt i32 %tw3se_slot0, {SLOTS}',
            '  %tw3se_slot = select i1 %tw3se_big, i32 0, i32 %tw3se_slot0',
            '  %tw3se_cbr = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle '
            '%tw3se_cb, i32 %tw3se_slot)',
        ] + [f'  %tw3se_o{c} = extractvalue %dx.types.CBufRet.f32 %tw3se_cbr, {k}' for k, c in enumerate('xyz')]
    return '\n'.join(out) + '\n', positions


def main(argv):
    if len(argv) == 2 and argv[0] == 'find':
        path = argv[1]
        files = ([os.path.join(path, f) for f in sorted(os.listdir(path)) if f.endswith('.ll')]
                 if os.path.isdir(path) else [path])
        summary = {}
        for f in files:
            body, _ = parse(open(f, encoding='utf-8', errors='replace').read())
            pos = find_positions(body)
            summary.setdefault(len(pos), []).append(os.path.basename(f))
            if len(files) == 1:
                for p in pos:
                    print('position:', ' '.join(p))
        for n, names in sorted(summary.items()):
            print(f'{len(names):3}  {n} position(s)  e.g. {names[0]}')
        return 0
    if len(argv) == 4 and argv[0] == 'patch':
        text, pos = patch(open(argv[1], encoding='utf-8', errors='replace').read(), argv[3])
        open(argv[2], 'w', encoding='utf-8', newline='\n').write(text)
        print(f'{argv[2]}: {len(pos)} position(s) patched')
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
