#!/usr/bin/env python3
# Faithful PMX 2.0/2.1 material-table dumper. Read-only. Prints the texture table and,
# per material, exactly what the PMX assigns: diffuse texture, sphere texture + mode,
# toon, draw flags, and the surface (index) count. This is the ground truth we re-apply.
import struct, sys, io
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

def read(f, n):
    b = f.read(n)
    if len(b) != n:
        raise EOFError(f"wanted {n} bytes, got {len(b)} at offset {f.tell()}")
    return b

def u8(f):  return struct.unpack('<B', read(f,1))[0]
def i8(f):  return struct.unpack('<b', read(f,1))[0]
def i32(f): return struct.unpack('<i', read(f,4))[0]
def f32(f): return struct.unpack('<f', read(f,4))[0]

def sized_index(f, size, signed=True):
    if size == 1: return struct.unpack('<b' if signed else '<B', read(f,1))[0]
    if size == 2: return struct.unpack('<h' if signed else '<H', read(f,2))[0]
    return struct.unpack('<i' if signed else '<I', read(f,4))[0]

def main(path):
    f = open(path, 'rb')
    magic = read(f,4)
    if magic[:3] != b'PMX':
        raise RuntimeError(f"not a PMX: magic={magic}")
    ver = f32(f)
    gcount = u8(f)
    g = [u8(f) for _ in range(gcount)]
    enc, addUV, vIdx, texIdx, matIdx, boneIdx, morphIdx, rbIdx = (g + [0]*8)[:8]
    encoding = 'utf-16-le' if enc == 0 else 'utf-8'

    def text(f):
        n = i32(f)
        return read(f, n).decode(encoding, errors='replace')

    name    = text(f); name_en = text(f)
    comment = text(f); comment_en = text(f)
    print(f"# PMX version {ver}  encoding={encoding}  addUV={addUV}")
    print(f"# model name: {name!r}")
    print(f"# texIdxSize={texIdx} matIdxSize={matIdx} boneIdxSize={boneIdx}")

    # --- vertices (parse fully to skip) ---
    vcount = i32(f)
    for _ in range(vcount):
        read(f, 4*8)                 # pos(3)+normal(3)+uv(2)
        read(f, 4*4*addUV)           # additional UVs
        wt = u8(f)
        if wt == 0:                  # BDEF1
            sized_index(f, boneIdx)
        elif wt == 1:                # BDEF2
            sized_index(f, boneIdx); sized_index(f, boneIdx); f32(f)
        elif wt == 2:                # BDEF4
            for _ in range(4): sized_index(f, boneIdx)
            read(f, 4*4)
        elif wt == 3:                # SDEF
            sized_index(f, boneIdx); sized_index(f, boneIdx); f32(f)
            read(f, 4*9)             # C, R0, R1
        elif wt == 4:                # QDEF (2.1)
            for _ in range(4): sized_index(f, boneIdx)
            read(f, 4*4)
        else:
            raise RuntimeError(f"unknown weight type {wt}")
        f32(f)                       # edge scale
    print(f"# vertices: {vcount}")

    # --- faces ---
    fcount = i32(f)                  # number of vertex indices (3 * triangles)
    for _ in range(fcount):
        sized_index(f, vIdx, signed=False)
    print(f"# face indices: {fcount}  ({fcount//3} triangles)")

    # --- textures ---
    tcount = i32(f)
    textures = [text(f) for _ in range(tcount)]
    print(f"\n# === TEXTURE TABLE ({tcount}) ===")
    for i, t in enumerate(textures):
        print(f"#  [{i:2d}] {t}")

    def texname(idx):
        if idx is None or idx < 0 or idx >= len(textures): return "(none)"
        return textures[idx]

    SPHERE = {0:'None', 1:'Mul', 2:'Add', 3:'SubTex'}
    FLAGS = [(0x01,'BothFace'),(0x02,'GroundShadow'),(0x04,'CastSelfShadow'),
             (0x08,'RecvSelfShadow'),(0x10,'DrawEdge'),(0x20,'VertexColor'),
             (0x40,'DrawPoint'),(0x80,'DrawLine')]

    # --- materials ---
    mcount = i32(f)
    print(f"\n# === MATERIALS ({mcount}) ===")
    idx_base = 0
    for mi in range(mcount):
        mname = text(f); mname_en = text(f)
        diffuse = struct.unpack('<4f', read(f,16))
        specular = struct.unpack('<3f', read(f,12))
        spow = f32(f)
        ambient = struct.unpack('<3f', read(f,12))
        flag = u8(f)
        edge_color = struct.unpack('<4f', read(f,16))
        edge_size = f32(f)
        tIndex  = sized_index(f, texIdx)
        spIndex = sized_index(f, texIdx)
        spMode  = u8(f)
        toonFlag = u8(f)
        if toonFlag == 0:
            toonIndex = sized_index(f, texIdx); toonStr = f"sep:{texname(toonIndex)}"
        else:
            toonIndex = u8(f); toonStr = f"common:toon{toonIndex+1:02d}.bmp"
        memo = text(f)
        nface = i32(f)
        ntri = nface // 3
        flagstr = '|'.join(n for b,n in FLAGS if flag & b) or '-'
        print(f"\n[{mi:2d}] {mname!r}  (en={mname_en!r})")
        print(f"      diffuse   : {texname(tIndex)}   (texIdx={tIndex})")
        print(f"      sphere    : {texname(spIndex)}   mode={SPHERE.get(spMode,spMode)} (spIdx={spIndex})")
        print(f"      toon      : {toonStr}")
        print(f"      flags     : {flagstr}")
        print(f"      diffuseRGBA: ({diffuse[0]:.2f},{diffuse[1]:.2f},{diffuse[2]:.2f},a={diffuse[3]:.2f})")
        print(f"      faces     : {nface} indices ({ntri} tris)  idxRange=[{idx_base},{idx_base+nface})")
        idx_base += nface

if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'LeMaline v1.0.pmx')
