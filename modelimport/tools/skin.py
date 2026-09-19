import numpy as np
from scipy.spatial.transform import Rotation

def fk(local, parent, deltas=None):
    n = len(local)
    world = np.zeros_like(local)
    for i in range(n):
        m = local[i] if deltas is None or i not in deltas else local[i] @ deltas[i]
        world[i] = m if parent[i] < 0 else world[parent[i]] @ m
    return world

def rotvec_matrix(rv):
    m = np.eye(4); m[:3, :3] = Rotation.from_rotvec(rv).as_matrix(); return m

def skin_matrices(world, invbind):
    return np.einsum('nij,njk->nik', world, invbind)

def blend(M, J, W):
    return np.einsum('nk,nkij->nij', W, M[J])

def apply(B, P):
    return np.einsum('nij,nj->ni', B[:, :3, :3], P) + B[:, :3, 3]

def apply_inverse(B, P):
    Binv = np.linalg.inv(B)
    return apply(Binv, P)
