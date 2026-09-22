import os, sys, collections
RD = r'C:\Users\rieng\Documents\GitHub\renderdoc\x64\Development'
os.add_dll_directory(RD); sys.path.insert(0, os.path.join(RD, 'pymodules'))
import renderdoc as rd
rd.InitialiseReplay(rd.GlobalEnvironment(), [])
cap = rd.OpenCaptureFile(); assert cap.OpenFile(sys.argv[1], '', None) == rd.ResultCode.Succeeded
st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None); assert st == rd.ResultCode.Succeeded
acts = []
def flat(a):
    for x in a: acts.append(x); flat(x.children)
flat(ctrl.GetRootActions())
draws = [a for a in acts if a.flags & rd.ActionFlags.Drawcall]
names = {r.resourceId: r.name for r in ctrl.GetResources()}
by = collections.OrderedDict()
for a in draws:
    rt = a.outputs[0] if len(a.outputs) and a.outputs[0] != rd.ResourceId.Null() else a.depthOut
    by.setdefault(names.get(rt, str(rt)), []).append(a)
for t, sel in by.items():
    if len(sel) < 100: continue
    adj = sum(1 for i in range(1, len(sel)) if sel[i].vertexOffset == sel[i-1].vertexOffset and sel[i].indexOffset == sel[i-1].indexOffset + sel[i-1].numIndices)
    def windowed(W):
        merged = 0; i = 0
        while i < len(sel):
            j = i
            while j + 1 < len(sel) and j - i < W - 1 and sel[j+1].vertexOffset == sel[i].vertexOffset: j += 1
            grp = sorted(sel[i:j+1], key=lambda a: a.indexOffset)
            merged += sum(1 for k in range(1, len(grp)) if grp[k].indexOffset == grp[k-1].indexOffset + grp[k-1].numIndices)
            i = j + 1
        return merged
    print("%-28s draws %5d | adjacent as emitted %4d | mergeable within a window of 16: %4d, 64: %4d, 256: %4d" % (t[:28], len(sel), adj, windowed(16), windowed(64), windowed(256)))
ctrl.Shutdown()
