import sys, os, time
import numpy as np
from scipy.spatial import cKDTree
from scipy.optimize import least_squares
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from retail_ref import load as load_retail
from skin import fk, rotvec_matrix, skin_matrices, blend, apply
from quickrender import render

MOVABLE = ['Bone_UpperArm_L', 'Bone_ForeArm_L', 'Bone_Hand_L', 'Bone_UpperArm_R', 'Bone_ForeArm_R', 'Bone_Hand_R',
           'Bone_Thigh_L', 'Bone_Calf_L', 'Bone_Foot_L', 'Bone_Thigh_R', 'Bone_Calf_R', 'Bone_Foot_R',
           'Bone_Spine_A', 'Bone_Spine_B', 'Bone_Spine_C', 'Bone_Neck', 'Bone_Head', 'Bone_Hips']
ARMS = MOVABLE[:6]
REG = {'Bone_Spine_A': 6.0, 'Bone_Spine_B': 6.0, 'Bone_Spine_C': 6.0, 'Bone_Neck': 4.0, 'Bone_Head': 4.0, 'Bone_Hips': 10.0,
       'Bone_Thigh_L': 3.0, 'Bone_Thigh_R': 3.0, 'Bone_Calf_L': 3.0, 'Bone_Calf_R': 3.0, 'Bone_Foot_L': 3.0, 'Bone_Foot_R': 3.0}

def retail_sample_mask(ref):
    names = ref['bone_names']
    wing = np.array([i for i, n in enumerate(names) if n.startswith('Bone_Wing')])
    wingw = np.where(np.isin(ref['J'], wing), ref['W'], 0).sum(1)
    return (ref['mesh_of'] != 0) & (wingw < 0.3)

def posed_world(ref, deltas_rv, idx):
    deltas = {b: rotvec_matrix(deltas_rv[k]) for k, b in enumerate(idx)}
    return fk(ref['local'], ref['parent'], deltas)

def main():
    ref = load_retail('SpiderMan2099[Hi]')
    fb = np.load('build/s99_aligned.npz')
    FP = fb['P']
    names = ref['bone_names']
    mask = retail_sample_mask(ref)
    RP, RJ, RW = ref['P'][mask], ref['J'][mask], ref['W'][mask]
    rng = np.random.default_rng(1)
    fsub = FP[rng.choice(len(FP), 12000, replace=False)]
    ftree = cKDTree(fsub)
    print('retail sample %d verts, fbx sample %d' % (len(RP), len(fsub)))

    deltas = np.zeros((len(names), 3))

    def solve(bones, start, nfev):
        idx = [names.index(n) for n in bones]
        reg = np.array([REG.get(n, 0.3) for n in bones])
        def pose_points(x):
            rv = x.reshape(-1, 3)
            world = posed_world(ref, rv, idx)
            M = skin_matrices(world, ref['invbind'])
            return apply(blend(M, RJ, RW), RP)
        def residuals(x):
            Q = pose_points(x)
            d1, _ = ftree.query(Q)
            d2, _ = cKDTree(Q).query(fsub)
            return np.concatenate([d1, d2 * 0.5, (x.reshape(-1, 3) * reg[:, None]).ravel()])
        x0 = start[idx].ravel()
        t = time.time()
        r0 = residuals(x0)
        print('stage %s: initial chamfer retail->fbx %.4f, fbx->retail %.4f' % (len(bones), r0[:len(RP)].mean(), r0[len(RP):len(RP) + len(fsub)].mean() * 2))
        res = least_squares(residuals, x0, method='trf', max_nfev=nfev, diff_step=1e-3, verbose=0)
        r1 = residuals(res.x)
        print('  after %.0fs: retail->fbx %.4f, fbx->retail %.4f (nfev %d)' % (time.time() - t, r1[:len(RP)].mean(), r1[len(RP):len(RP) + len(fsub)].mean() * 2, res.nfev))
        rv = res.x.reshape(-1, 3)
        for k, n in enumerate(bones):
            print('  %-18s rot %6.1f deg  axis %s' % (n, np.degrees(np.linalg.norm(rv[k])), (rv[k] / (np.linalg.norm(rv[k]) + 1e-9)).round(2)))
        out = start.copy(); out[idx] = rv
        return out

    deltas = solve(ARMS, deltas, 60)
    deltas = solve(MOVABLE, deltas, 40)
    idx = [names.index(n) for n in MOVABLE]; rv = deltas[idx]
    np.savez('build/s99_pose.npz', deltas=deltas)
    world = posed_world(ref, rv, idx)
    M = skin_matrices(world, ref['invbind'])
    allQ = apply(blend(M, ref['J'], ref['W']), ref['P'])
    render([(FP, fb['tris'], (200, 60, 60)), (allQ, ref['tris'], (60, 120, 220))], 'build/pose.png', views=('front', 'side'))

if __name__ == '__main__':
    main()
