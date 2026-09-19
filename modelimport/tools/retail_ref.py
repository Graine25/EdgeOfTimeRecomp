import sys, os, struct
import numpy as np
sys.path.insert(0, r'C:\Users\rieng\Documents\GitHub\reeot-dni\reference\eot_tools\SPIDER-MAN EDGE OF TIME TOOLS\tools')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import x360_geo as G
from sd_export_gltf import load_skeletons, parse_skeleton
from pakwriter import Pak

BG = 'D:/EOT_Extract/extracted/BaseGameplay.ext'

def unpack_hend3n(u):
    x = (u & 0x7FF).astype(np.int64); y = ((u >> 11) & 0x7FF).astype(np.int64); z = ((u >> 22) & 0x3FF).astype(np.int64)
    x = np.where(x > 1023, x - 2048, x); y = np.where(y > 1023, y - 2048, y); z = np.where(z > 511, z - 1024, z)
    return np.stack([x / 1023.0, y / 1023.0, z / 511.0], 1)

def unpack_dec3n(u):
    x = (u & 0x3FF).astype(np.int64); y = ((u >> 10) & 0x3FF).astype(np.int64); z = ((u >> 20) & 0x3FF).astype(np.int64)
    w = (u >> 30).astype(np.int64)
    x = np.where(x > 511, x - 1024, x); y = np.where(y > 511, y - 1024, y); z = np.where(z > 511, z - 1024, z)
    return np.stack([x / 511.0, y / 511.0, z / 511.0], 1), w

def load(name, pak_path=BG, skeleton='SpiderManAmazing[Hi]'):
    pak = Pak(pak_path)
    d = pak.d
    m = {mm[0]: mm for mm in G.get_models(d)}[name]
    _, vb, vbsize, ib, ibsize = m
    descs = G.get_descriptors(d, name)
    P = []; N = []; T = []; TW = []; UV = []; J = []; W = []; tris = []; mesh_of = []
    base_index = 0
    for mi, dd in enumerate(descs):
        o = G.offsets(dd['fmt']); st = dd['stride']; nv = dd['nv']
        raw = np.frombuffer(d[vb + dd['vboff']:vb + dd['vboff'] + nv * st], dtype=np.uint8).reshape(nv, st)
        P.append(raw[:, 0:12].copy().view('>f4').reshape(nv, 3).astype(np.float64))
        N.append(unpack_hend3n(raw[:, o['nrm']:o['nrm'] + 4].copy().view('>u4').reshape(nv)) if o['nrm'] is not None else np.zeros((nv, 3)))
        if o['tan'] is not None:
            t, w = unpack_dec3n(raw[:, o['tan']:o['tan'] + 4].copy().view('>u4').reshape(nv)); T.append(t); TW.append(w)
        else:
            T.append(np.zeros((nv, 3))); TW.append(np.zeros(nv, np.int64))
        UV.append(raw[:, o['uv']:o['uv'] + 4].copy().view('>i2').reshape(nv, 2) / 1024.0 if o['uv'] is not None else np.zeros((nv, 2)))
        if o['skin'] is not None:
            pal = np.array(dd['pal'], np.int64)
            j = raw[:, o['skin']:o['skin'] + 4][:, ::-1].astype(np.int64); w = raw[:, o['skin'] + 4:o['skin'] + 8][:, ::-1].astype(np.float64) / 255.0
            J.append(pal[j]); W.append(w)
        else:
            J.append(np.zeros((nv, 4), np.int64)); W.append(np.tile([1.0, 0, 0, 0], (nv, 1)))
        t = np.array(G.decode_indices(d, ib, dd), np.int64).reshape(-1, 3) + base_index
        tris.append(t); mesh_of.append(np.full(nv, mi)); base_index += nv
    sk = load_skeletons(pak_path)
    skel = parse_skeleton(*sk[skeleton])
    invbind = np.array([ib.T for ib in skel.invbind]); local = np.array([l.T for l in skel.local])
    world = np.array([np.linalg.inv(ib) for ib in invbind])
    return dict(P=np.concatenate(P), N=np.concatenate(N), T=np.concatenate(T), TW=np.concatenate(TW), UV=np.concatenate(UV),
                J=np.concatenate(J), W=np.concatenate(W), tris=np.concatenate(tris), mesh_of=np.concatenate(mesh_of),
                descs=descs, bone_names=skel.names, parent=np.array(skel.parent), world=world, invbind=invbind, local=local)

if __name__ == '__main__':
    ref = load(sys.argv[1] if len(sys.argv) > 1 else 'SpiderMan2099[Hi]')
    print('verts', len(ref['P']), 'tris', len(ref['tris']), 'bbox', ref['P'].min(0).round(3), ref['P'].max(0).round(3))
    print('bones', len(ref['bone_names']))
    for i, n in enumerate(ref['bone_names']):
        print('  %3d %-28s parent %3d pos %s' % (i, n, ref['parent'][i], ref['world'][i][:3, 3].round(3)))
    print('normal len mean %.3f, tangent len mean %.3f, n.t mean %.3f, tw values %s' % (
        np.linalg.norm(ref['N'], axis=1).mean(), np.linalg.norm(ref['T'], axis=1).mean(), abs((ref['N'] * ref['T']).sum(1)).mean(), np.unique(ref['TW'])))
