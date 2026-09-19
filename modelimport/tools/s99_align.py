import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from quickrender import render
from retail_ref import load as load_retail

DROP = {'GeometryNode_71'}

def main():
    z = np.load('build/s99_meshes.npz')
    names = [n for n in z['names'] if n not in DROP]
    P = []; N = []; UV = []; tris = []; mesh_of = []; base = 0
    for i, n in enumerate(names):
        p = z[n + '_P']; P.append(p); N.append(z[n + '_N']); UV.append(z[n + '_UV'])
        tris.append(z[n + '_tris'] + base); mesh_of.append(np.full(len(p), i)); base += len(p)
    P = np.concatenate(P); N = np.concatenate(N); UV = np.concatenate(UV); tris = np.concatenate(tris); mesh_of = np.concatenate(mesh_of)
    ref = load_retail('SpiderMan2099[Hi]')
    rp = ref['P']
    lo, hi = P.min(0), P.max(0); rlo, rhi = rp.min(0), rp.max(0)
    scale = (rhi[1] - rlo[1]) / (hi[1] - lo[1])
    P = (P - [(lo[0] + hi[0]) / 2, lo[1], (lo[2] + hi[2]) / 2]) * scale + [0, rlo[1], (rlo[2] + rhi[2]) / 2]
    print('scale %.4f; aligned bbox %s .. %s; retail %s .. %s' % (scale, P.min(0).round(3), P.max(0).round(3), rlo.round(3), rhi.round(3)))
    for tag, Q in (('fbx', P), ('retail', rp)):
        col = Q[(abs(Q[:, 0]) < 0.02) & (Q[:, 1] > 0.3) & (Q[:, 1] < 1.2)]
        print('  %s crotch ~ y=%.3f, width at hips %.3f, shoulder width (y 1.40..1.55) %.3f' % (
            tag, col[:, 1].min() if len(col) else -1,
            np.ptp(Q[(Q[:, 1] > 0.95) & (Q[:, 1] < 1.05)][:, 0]), np.ptp(Q[(Q[:, 1] > 1.40) & (Q[:, 1] < 1.55)][:, 0])))
    np.savez('build/s99_aligned.npz', P=P, N=N, UV=UV, tris=tris, mesh_of=mesh_of, mesh_names=np.array(names), scale=scale)
    render([(P, tris, (200, 60, 60)), (rp, ref['tris'], (60, 120, 220))], 'build/align.png', views=('front', 'side'))

if __name__ == '__main__':
    main()
