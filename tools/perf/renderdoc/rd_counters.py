import os, sys, collections
RD = r'C:\Users\rieng\Documents\GitHub\renderdoc\x64\Development'
os.add_dll_directory(RD)
sys.path.insert(0, os.path.join(RD, 'pymodules'))
import renderdoc as rd

path = sys.argv[1]
topn = int(sys.argv[2]) if len(sys.argv) > 2 else 40
rd.InitialiseReplay(rd.GlobalEnvironment(), [])
cap = rd.OpenCaptureFile()
st = cap.OpenFile(path, '', None)
assert st == rd.ResultCode.Succeeded, st
st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
assert st == rd.ResultCode.Succeeded, st

acts = []
def flat(a):
    for x in a:
        acts.append(x)
        flat(x.children)
flat(ctrl.GetRootActions())
draws = [a for a in acts if a.flags & rd.ActionFlags.Drawcall]
print("events", len(acts), "draws", len(draws))

names = {r.resourceId: r.name for r in ctrl.GetResources()}
def nm(rid):
    return names.get(rid, str(rid))

counters = [rd.GPUCounter.EventGPUDuration, rd.GPUCounter.PSInvocations, rd.GPUCounter.SamplesPassed]
res = ctrl.FetchCounters(counters)
dur = collections.defaultdict(float); psi = collections.defaultdict(int); smp = collections.defaultdict(int)
for r in res:
    if r.counter == rd.GPUCounter.EventGPUDuration:
        dur[r.eventId] += r.value.d * 1000.0
    elif r.counter == rd.GPUCounter.PSInvocations:
        psi[r.eventId] += r.value.u64
    elif r.counter == rd.GPUCounter.SamplesPassed:
        smp[r.eventId] += r.value.u64

total = sum(dur.values())
print("total GPU (sum of events) %.2f ms; draws %.2f ms" % (total, sum(dur[a.eventId] for a in draws)))

bytarget = collections.defaultdict(lambda: [0, 0.0, 0])
for a in draws:
    rt = a.outputs[0] if len(a.outputs) and a.outputs[0] != rd.ResourceId.Null() else a.depthOut
    k = nm(rt)
    bytarget[k][0] += 1; bytarget[k][1] += dur[a.eventId]; bytarget[k][2] += psi[a.eventId]
print("\n--- by target (draws, ms, PS invocations M) ---")
for k, v in sorted(bytarget.items(), key=lambda kv: -kv[1][1])[:14]:
    print("%-40s %5d draws %7.2f ms %8.1f M ps" % (k[:40], v[0], v[1], v[2] / 1e6))

top = sorted(draws, key=lambda a: -dur[a.eventId])[:topn]
print("\n--- top %d draws ---" % topn)
for a in top:
    ctrl.SetFrameEvent(a.eventId, False)
    ps = ctrl.GetPipelineState()
    vs = ps.GetShader(rd.ShaderStage.Vertex); pxs = ps.GetShader(rd.ShaderStage.Pixel)
    rt = a.outputs[0] if len(a.outputs) and a.outputs[0] != rd.ResourceId.Null() else a.depthOut
    print("eid %5d %7.3f ms  ps %9.2f M  smp %8.1f M  idx %6d  vs %s ps %s -> %s" % (
        a.eventId, dur[a.eventId], psi[a.eventId] / 1e6, smp[a.eventId] / 1e6, a.numIndices,
        nm(vs)[-10:], nm(pxs)[-10:], nm(rt)[:28]))

want = sys.argv[3:] if len(sys.argv) > 3 else []
if want:
    print("\n--- draws by pixel shader fragment ---")
    pass
ctrl.Shutdown()
cap.Shutdown()
