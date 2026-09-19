import numpy as np

W, H = 2552, 1440
_c0 = np.frombuffer(bytes.fromhex(open('C:/tmp/s99b_vsc.hex').read().strip()), '<f4').reshape(-1, 4).astype(np.float64)
_c1 = np.frombuffer(bytes.fromhex(open('C:/tmp/s99b_vsc1.hex').read().strip()), '<f4').reshape(-1, 4).astype(np.float64)
_pals = np.load('build/palettes.npy', allow_pickle=True)
PAL0, PAL1 = list(_pals[0]), list(_pals[1])
WORLD = _c0[4:7]
VP = _c0[0:4]

def bone_matrices():
    M = {}
    for i, b in enumerate(PAL0):
        M[b] = _c0[66 + 3 * i:66 + 3 * i + 3]
    for i, b in enumerate(PAL1):
        if b not in M:
            M[b] = _c1[3 * i:3 * i + 3]
    return M

def skin(P, J, W, M, n_infl=3):
    order = np.argsort(-W, axis=1)[:, :n_infl]
    Jl = np.take_along_axis(J, order, 1); Wl = np.take_along_axis(W, order, 1)
    Wl = Wl / np.maximum(Wl.sum(1, keepdims=True), 1e-12)
    P1 = np.concatenate([P, np.ones((len(P), 1))], 1)
    ids = sorted(M.keys())
    table = np.zeros((max(ids) + 1, 3, 4));
    for b in ids:
        table[b] = M[b]
    out = np.zeros((len(P), 3))
    for i in range(n_infl):
        m = table[Jl[:, i]]
        out += Wl[:, i:i + 1] * np.einsum('nij,nj->ni', m, P1)
    return out

def project(S):
    S1 = np.concatenate([S, np.ones((len(S), 1))], 1)
    Wp = S1 @ WORLD.T
    Wp1 = np.concatenate([Wp, np.ones((len(S), 1))], 1)
    clip = Wp1 @ VP.T
    w = clip[:, 3]
    z = (-clip[:, 2] + w) / w
    sx = (clip[:, 0] / w * 0.5 + 0.5) * W
    sy = (1 - (clip[:, 1] / w * 0.5 + 0.5)) * H
    return sx, sy, z, w
