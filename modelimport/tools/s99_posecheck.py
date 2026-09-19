import sys
import numpy as np
sys.path.insert(0, 'tools')
import capture_pose as cp
import s99_posetest as pt
from retail_ref import load as load_retail
from skin import fk, rotvec_matrix, skin_matrices

def matrices_from_deltas(ref, deltas):
    world = fk(ref['local'], ref['parent'], {b: rotvec_matrix(rv) for b, rv in deltas.items() if np.any(rv)})
    M = skin_matrices(world, ref['invbind'])
    return {b: M[b, :3, :] for b in range(len(M))}

def turned(deg):
    a = np.radians(deg); c, s = np.cos(a), np.sin(a)
    R = np.array([[c, 0, s, 0], [0, 1, 0, 0], [-s, 0, c, 0], [0, 0, 0, 1]])
    return cp.WORLD @ R

def run(rig, poses, views=((0, 'back'), (180, 'front'))):
    names = list(rig['mesh_names'])
    mat_of_mesh = {names.index('GeometryNode_5'): 0, names.index('GeometryNode_41'): 0,
                   names.index('GeometryNode_23'): 1, names.index('GeometryNode_56'): 1}
    tris = rig['tris'][:, [0, 2, 1]]; mesh_of = rig['mesh_of']
    tri_mat = np.array([mat_of_mesh[m] for m in mesh_of[tris[:, 0]]])
    order = np.argsort(tri_mat, kind='stable'); tris = tris[order]; tri_mat = tri_mat[order]
    world0 = cp.WORLD.copy()
    total = 0
    for pname, M in poses:
        for deg, vname in views:
            cp.WORLD = turned(deg)
            n_vis, n_hole, _ = pt.pose_test(rig['P'], rig['J'], rig['W'], tris, tri_mat, M=M,
                                            out=f'build/posecheck_{pname}_{vname}.png', label=f'{pname:12s} {vname:5s}: ')
            total += n_hole
    cp.WORLD = world0
    print('total see-through pixels over all poses/views:', total)
    return total

def synthetic_poses(ref):
    bn = list(ref['bone_names'])
    b = lambda n: bn.index(n)
    fbx = np.load('build/s99_pose.npz')['deltas']
    poses = []
    poses.append(('captured', cp.bone_matrices()))
    poses.append(('fbx_stance', matrices_from_deltas(ref, {i: fbx[i] for i in range(len(bn))})))
    for axis in range(3):
        for ang in (60, 100):
            rv = np.zeros(3); rv[axis] = np.radians(ang)
            d = {b('Bone_ForeArm_L'): rv, b('Bone_ForeArm_R'): rv}
            poses.append((f'elbow{axis}_{ang}', matrices_from_deltas(ref, d)))
    for axis in range(3):
        rv = np.zeros(3); rv[axis] = np.radians(50)
        d = {b('Bone_UpperArm_L'): rv, b('Bone_UpperArm_R'): rv}
        poses.append((f'shoulder{axis}_50', matrices_from_deltas(ref, d)))
    for axis in range(3):
        rv = np.zeros(3); rv[axis] = np.radians(60)
        d = {b('Bone_Thigh_L'): rv, b('Bone_Calf_R'): rv}
        poses.append((f'leg{axis}_60', matrices_from_deltas(ref, d)))
    return poses

if __name__ == '__main__':
    src = sys.argv[1] if len(sys.argv) > 1 else 'build/s99_rigged.npz'
    rig = np.load(src, allow_pickle=True)
    ref = load_retail('SpiderMan2099[Hi]')
    run(rig, synthetic_poses(ref))
