import os, sys, collections
RD = r'C:\Users\rieng\Documents\GitHub\renderdoc\x64\Development'
os.add_dll_directory(RD)
sys.path.insert(0, os.path.join(RD, 'pymodules'))
import renderdoc as rd
path = sys.argv[1]
targets = sys.argv[2:] or None
rd.InitialiseReplay(rd.GlobalEnvironment(), [])
cap = rd.OpenCaptureFile()
assert cap.OpenFile(path, '', None) == rd.ResultCode.Succeeded
st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
assert st == rd.ResultCode.Succeeded
acts = []
def flat(a):
    for x in a:
        acts.append(x); flat(x.children)
flat(ctrl.GetRootActions())
draws = [a for a in acts if a.flags & rd.ActionFlags.Drawcall]
names = {r.resourceId: r.name for r in ctrl.GetResources()}
nm = lambda rid: names.get(rid, str(rid))
bytarget = collections.OrderedDict()
for a in draws:
    rt = a.outputs[0] if len(a.outputs) and a.outputs[0] != rd.ResourceId.Null() else a.depthOut
    bytarget.setdefault(nm(rt), []).append(a)
for tname, sel in bytarget.items():
    if targets and not any(t in tname for t in targets):
        continue
    if len(sel) < 50:
        continue
    keys = []
    for a in sel:
        ctrl.SetFrameEvent(a.eventId, False)
        ps = ctrl.GetPipelineState()
        vbs = ps.GetVBuffers()
        vb = (str(vbs[0].resourceId), vbs[0].byteOffset) if len(vbs) else ("-", 0)
        ib = ps.GetIBuffer()
        key = (vb[0], vb[1], str(ib.resourceId), a.indexOffset, a.numIndices, a.vertexOffset, str(ps.GetGraphicsPipelineObject()))
        keys.append((a, key))
    seen = collections.Counter(k for _, k in keys)
    uniq = sum(1 for k in seen)
    repeats = sum(c - 1 for c in seen.values())
    idx = [a.numIndices for a, _ in keys]
    tiny = sum(1 for n in idx if n <= 192)
    small = sum(1 for n in idx if 192 < n <= 768)
    vbcount = len({k[0] for _, k in keys})
    pso = len({k[6] for _, k in keys})
    idx_sorted = sorted(idx)
    p50 = idx_sorted[len(idx_sorted) // 2]; p90 = idx_sorted[int(0.9 * len(idx_sorted))]
    top = seen.most_common(5)
    print("== %s: %d draws | unique meshes %d, repeats (copies) %d | indices p50 %d p90 %d, <=192: %d, 193-768: %d | vertex buffers %d, PSOs %d" % (
        tname, len(sel), uniq, repeats, p50, p90, tiny, small, vbcount, pso))
    print("   most repeated meshes (vb, offset, indices) x times:", [(k[0][-6:], k[1], k[4], c) for k, c in top])
    byvb = collections.Counter(k[0] for _, k in keys)
    print("   draws per vertex buffer (top 6):", [(v[-6:], c) for v, c in byvb.most_common(6)])
ctrl.Shutdown()
