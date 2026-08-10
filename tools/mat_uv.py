#!/usr/bin/env python3
# Dump each material's name, diffuse texture, and the UV bounding box of its vertices, plus
# (1-v) flipped bounds. Read-only PMX walk. Lets us see which atlas region a material samples.
import struct, sys

def main(path):
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
    vcount=i32(); uvs=[]
    for _ in range(vcount):
        rd(24)  # pos(3)+normal(3)
        u,v=struct.unpack('<2f', rd(8)); uvs.append((u,v))
        rd(16*addUV); wt=u8()
        if wt==0: sidx(boneIdx)
        elif wt==1: sidx(boneIdx);sidx(boneIdx);f32()
        elif wt==2: [sidx(boneIdx) for _ in range(4)];rd(16)
        elif wt==3: sidx(boneIdx);sidx(boneIdx);f32();rd(36)
        elif wt==4: [sidx(boneIdx) for _ in range(4)];rd(16)
        f32()
    fcount=i32(); faces=[sidx(vIdx,signed=False) for _ in range(fcount)]
    textures=[text() for _ in range(i32())]
    def tn(i): return textures[i].split('/')[-1] if 0<=i<len(textures) else '(none)'
    base=0
    for _ in range(i32()):
        nm=text(); text(); rd(16+12+4+12); u8(); rd(16+4)
        ti=sidx(texIdx); sidx(texIdx); u8()
        if u8()==0: sidx(texIdx)
        else: u8()
        text(); nf=i32()
        vids=set(faces[base:base+nf]); base+=nf
        if not vids: continue
        us=[uvs[v][0] for v in vids]; vs=[uvs[v][1] for v in vids]
        umin,umax=min(us),max(us); vmin,vmax=min(vs),max(vs)
        print(f"{nm:16s} tex={tn(ti):28s} u[{umin:.3f},{umax:.3f}] v[{vmin:.3f},{vmax:.3f}]  flipped v[{1-vmax:.3f},{1-vmin:.3f}]")

if __name__=='__main__':
    main(sys.argv[1])
