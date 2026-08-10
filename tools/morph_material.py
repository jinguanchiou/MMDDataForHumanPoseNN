#!/usr/bin/env python3
# For each named morph, report which MATERIAL(s) its vertices belong to (so we know which
# submesh to overlay). Read-only PMX walk.
import struct, sys

def main(path, targets):
    f = open(path, 'rb'); rd=lambda n: f.read(n)
    u8=lambda:struct.unpack('<B',rd(1))[0]; i32=lambda:struct.unpack('<i',rd(4))[0]; f32=lambda:struct.unpack('<f',rd(4))[0]
    def sidx(s,signed=True):
        if s==1: return struct.unpack('<b' if signed else '<B',rd(1))[0]
        if s==2: return struct.unpack('<h' if signed else '<H',rd(2))[0]
        return struct.unpack('<i' if signed else '<I',rd(4))[0]
    assert rd(4)[:3]==b'PMX'; f32()
    g=[u8() for _ in range(u8())]; enc,addUV,vIdx,texIdx,matIdx,boneIdx,morphIdx,rbIdx=(g+[0]*8)[:8]
    encoding='utf-16-le' if enc==0 else 'utf-8'
    def text(): return rd(i32()).decode(encoding,'replace')
    text();text();text();text()
    vcount=i32()
    for _ in range(vcount):
        rd(32); rd(16*addUV); wt=u8()
        if wt==0: sidx(boneIdx)
        elif wt==1: sidx(boneIdx);sidx(boneIdx);f32()
        elif wt==2: [sidx(boneIdx) for _ in range(4)];rd(16)
        elif wt==3: sidx(boneIdx);sidx(boneIdx);f32();rd(36)
        elif wt==4: [sidx(boneIdx) for _ in range(4)];rd(16)
        f32()
    fcount=i32(); faces=[sidx(vIdx,signed=False) for _ in range(fcount)]
    textures=[text() for _ in range(i32())]
    def tn(i): return textures[i] if 0<=i<len(textures) else '(none)'
    mats=[]; base=0
    for _ in range(i32()):
        text();text(); rd(16+12+4+12); u8(); rd(16+4)
        ti=sidx(texIdx); sidx(texIdx); u8()
        if u8()==0: sidx(texIdx)
        else: u8()
        text(); nf=i32(); mats.append((tn(ti),base,nf)); base+=nf
    # build vertex -> material (by which face range references it)
    vmat={}
    for mi,(tex,b,nf) in enumerate(mats):
        for fi in range(b,b+nf):
            vmat[faces[fi]]=mi
    # bones (skip)
    for _ in range(i32()):
        text();text();rd(12);sidx(boneIdx);i32()
        fl=struct.unpack('<H',rd(2))[0]
        if fl&0x0001: sidx(boneIdx)
        else: rd(12)
        if fl&0x0300: sidx(boneIdx);f32()
        if fl&0x0400: rd(12)
        if fl&0x0800: rd(24)
        if fl&0x2000: i32()
        if fl&0x0020:
            sidx(boneIdx);i32();f32()
            for _ in range(i32()):
                sidx(boneIdx)
                if u8(): rd(24)
    # morphs
    for _ in range(i32()):
        nm=text(); text(); u8(); t=u8(); cnt=i32(); verts=[]
        for _ in range(cnt):
            if t in (0,9): sidx(morphIdx);f32()
            elif t==1: verts.append(sidx(vIdx,signed=False)); rd(12)
            elif t==2: sidx(boneIdx);rd(28)
            elif 3<=t<=7: verts.append(sidx(vIdx,signed=False)); rd(16)
            elif t==8: sidx(matIdx);u8();rd(112)
            elif t==10: sidx(rbIdx);rd(25)
        if nm in targets:
            from collections import Counter
            c=Counter(mats[vmat[v]][0] for v in verts if v in vmat)
            print(f"morph {nm!r} (type {t}, {len(verts)} verts) -> materials:")
            for tex,n in c.most_common(): print(f"    {n:5d} verts  tex={tex}")

if __name__=='__main__':
    main(sys.argv[1], set(sys.argv[2:]))
