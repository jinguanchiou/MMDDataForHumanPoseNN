#!/usr/bin/env python3
# Read PMX vertex positions + faces, then for each material's index range compute the
# bind-pose bounding box of the vertices it references. Tells us whether the expression
# overlay materials (Eff_Pink/Aozame/Angry/Bickle/Sweat) are degenerate at rest (hidden
# until a morph expands them) or have real extent (and would show in a static neutral pose).
import struct, sys

def read(f,n):
    b=f.read(n)
    if len(b)!=n: raise EOFError
    return b
def u8(f): return read(f,1)[0]
def i32(f): return struct.unpack('<i',read(f,4))[0]
def f32(f): return struct.unpack('<f',read(f,4))[0]
def sidx(f,sz,signed=True):
    if sz==1: return struct.unpack('<b' if signed else '<B',read(f,1))[0]
    if sz==2: return struct.unpack('<h' if signed else '<H',read(f,2))[0]
    return struct.unpack('<i' if signed else '<I',read(f,4))[0]

def main(path):
    f=open(path,'rb'); read(f,4); f32(f); gc=u8(f); g=[u8(f) for _ in range(gc)]
    enc,addUV,vIdx,texIdx,matIdx,boneIdx=g[0],g[1],g[2],g[3],g[4],g[5]
    encoding='utf-16-le' if enc==0 else 'utf-8'
    def text(f):
        n=i32(f); return read(f,n).decode(encoding,'replace')
    for _ in range(4): text(f)  # names/comments
    vcount=i32(f)
    pos=[None]*vcount
    for i in range(vcount):
        x,y,z=struct.unpack('<3f',read(f,12)); read(f,12+8); read(f,16*addUV)
        pos[i]=(x,y,z)
        wt=u8(f)
        if wt==0: sidx(f,boneIdx)
        elif wt==1: sidx(f,boneIdx); sidx(f,boneIdx); f32(f)
        elif wt==2:
            for _ in range(4): sidx(f,boneIdx)
            read(f,16)
        elif wt==3:
            sidx(f,boneIdx); sidx(f,boneIdx); f32(f); read(f,36)
        elif wt==4:
            for _ in range(4): sidx(f,boneIdx)
            read(f,16)
        f32(f)
    fcount=i32(f)
    faces=struct.unpack(f'<{fcount}'+('B' if vIdx==1 else 'H' if vIdx==2 else 'I'), read(f,fcount*vIdx))
    tcount=i32(f)
    for _ in range(tcount): text(f)
    mcount=i32(f)
    base=0
    for mi in range(mcount):
        mname=text(f); text(f); read(f,16+12+4+12); u8(f); read(f,16+4)
        sidx(f,texIdx); sidx(f,texIdx); u8(f)
        tf=u8(f); sidx(f,texIdx) if tf==0 else u8(f); text(f); nf=i32(f)
        # bounds of referenced verts
        xs=ys=zs=None
        mnx=mny=mnz=1e30; mxx=mxy=mxz=-1e30
        for k in range(base, base+nf):
            x,y,z=pos[faces[k]]
            mnx=min(mnx,x);mny=min(mny,y);mnz=min(mnz,z)
            mxx=max(mxx,x);mxy=max(mxy,y);mxz=max(mxz,z)
        ext=(mxx-mnx, mxy-mny, mxz-mnz)
        diag=(ext[0]**2+ext[1]**2+ext[2]**2)**0.5
        ctr=((mnx+mxx)/2,(mny+mxy)/2,(mnz+mxz)/2)
        print(f"[{mi:2d}] {mname:16s} tris={nf//3:6d}  extent=({ext[0]:.3f},{ext[1]:.3f},{ext[2]:.3f}) diag={diag:.3f}  center=({ctr[0]:.2f},{ctr[1]:.2f},{ctr[2]:.2f})")
        base+=nf

if __name__=='__main__':
    main(sys.argv[1])
