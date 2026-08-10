#!/usr/bin/env python3
# Read-only PMX morph dumper: walks past vertices/faces/textures/materials/bones to the
# morph table and prints each morph's name + TYPE (Group/Position/Bone/UV/Material/...).
import struct, sys

def main(path):
    f = open(path, 'rb')
    rd = lambda n: f.read(n)
    u8 = lambda: struct.unpack('<B', rd(1))[0]
    i32 = lambda: struct.unpack('<i', rd(4))[0]
    f32 = lambda: struct.unpack('<f', rd(4))[0]
    def sidx(size, signed=True):
        if size == 1: return struct.unpack('<b' if signed else '<B', rd(1))[0]
        if size == 2: return struct.unpack('<h' if signed else '<H', rd(2))[0]
        return struct.unpack('<i' if signed else '<I', rd(4))[0]

    assert rd(4)[:3] == b'PMX'
    f32()  # version
    g = [u8() for _ in range(u8())]
    enc, addUV, vIdx, texIdx, matIdx, boneIdx, morphIdx, rbIdx = (g + [0]*8)[:8]
    encoding = 'utf-16-le' if enc == 0 else 'utf-8'
    def text():
        n = i32(); return rd(n).decode(encoding, 'replace')
    text(); text(); text(); text()  # names + comments

    # vertices
    for _ in range(i32()):
        rd(4*8); rd(4*4*addUV); wt = u8()
        if wt == 0: sidx(boneIdx)
        elif wt == 1: sidx(boneIdx); sidx(boneIdx); f32()
        elif wt == 2: [sidx(boneIdx) for _ in range(4)]; rd(16)
        elif wt == 3: sidx(boneIdx); sidx(boneIdx); f32(); rd(36)
        elif wt == 4: [sidx(boneIdx) for _ in range(4)]; rd(16)
        f32()
    for _ in range(i32()): sidx(vIdx, signed=False)          # faces
    for _ in range(i32()): text()                            # textures
    # materials
    for _ in range(i32()):
        text(); text(); rd(16+12+4+12); u8(); rd(16+4)
        sidx(texIdx); sidx(texIdx); u8()
        if u8() == 0: sidx(texIdx)
        else: u8()
        text(); i32()
    # bones
    for _ in range(i32()):
        text(); text(); rd(12); sidx(boneIdx); i32()
        fl = struct.unpack('<H', rd(2))[0]
        if fl & 0x0001: sidx(boneIdx)
        else: rd(12)
        if fl & 0x0300: sidx(boneIdx); f32()
        if fl & 0x0400: rd(12)
        if fl & 0x0800: rd(24)
        if fl & 0x2000: i32()
        if fl & 0x0020:
            sidx(boneIdx); i32(); f32()
            for _ in range(i32()):
                sidx(boneIdx)
                if u8(): rd(24)

    TYPES = {0:'Group',1:'Position(vertex)',2:'Bone',3:'UV',4:'AddUV1',5:'AddUV2',
             6:'AddUV3',7:'AddUV4',8:'Material',9:'Flip',10:'Impulse'}
    n = i32()
    print(f"# {n} morphs:")
    for mi in range(n):
        nm = text(); text(); panel = u8(); t = u8(); cnt = i32()
        for _ in range(cnt):
            if t == 0 or t == 9: sidx(morphIdx); f32()
            elif t == 1: sidx(vIdx, signed=False); rd(12)
            elif t == 2: sidx(boneIdx); rd(28)
            elif 3 <= t <= 7: sidx(vIdx, signed=False); rd(16)
            elif t == 8: sidx(matIdx); u8(); rd(112)
            elif t == 10: sidx(rbIdx); rd(25)
        print(f"  [{mi:2d}] {TYPES.get(t,t):16s}  {nm!r}")

if __name__ == '__main__':
    main(sys.argv[1])
