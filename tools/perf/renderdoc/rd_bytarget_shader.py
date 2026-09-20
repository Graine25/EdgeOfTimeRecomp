import os, sys, collections
RD = r'C:\Users\rieng\Documents\GitHub\renderdoc\x64\Development'
os.add_dll_directory(RD)
sys.path.insert(0, os.path.join(RD, 'pymodules'))
import renderdoc as rd
path = sys.argv[1]
targets = sys.argv[2].split(',')
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
res = ctrl.FetchCounters([rd.GPUCounter.EventGPUDuration, rd.GPUCounter.PSInvocations, rd.GPUCounter.SamplesPassed])
dur = collections.defaultdict(float); psi = collections.defaultdict(int); smp = collections.defaultdict(int)
for r in res:
    if r.counter == rd.GPUCounter.EventGPUDuration: dur[r.eventId] += r.value.d * 1000.0
    elif r.counter == rd.GPUCounter.PSInvocations: psi[r.eventId] += r.value.u64
    else: smp[r.eventId] += r.value.u64
for t in targets:
    sel = []
    for a in draws:
        rt = a.outputs[0] if len(a.outputs) and a.outputs[0] != rd.ResourceId.Null() else a.depthOut
        if t in nm(rt): sel.append(a)
    tally = collections.defaultdict(lambda: [0, 0.0, 0, 0, 0])
    for a in sel:
        ctrl.SetFrameEvent(a.eventId, False)
        ps = ctrl.GetPipelineState()
        key = (nm(ps.GetShader(rd.ShaderStage.Vertex))[-10:], nm(ps.GetShader(rd.ShaderStage.Pixel))[-10:])
        v = tally[key]; v[0] += 1; v[1] += dur[a.eventId]; v[2] += psi[a.eventId]; v[3] += smp[a.eventId]; v[4] += a.numIndices
    print("\n=== target '%s': %d draws, %.2f ms, %.1f M ps" % (t, len(sel), sum(dur[a.eventId] for a in sel), sum(psi[a.eventId] for a in sel) / 1e6))
    for k, v in sorted(tally.items(), key=lambda kv: -kv[1][1])[:14]:
        print("  vs %s ps %s  %4d draws %6.2f ms  ps %7.2f M  passed %7.2f M  idx %7d" % (k[0], k[1], v[0], v[1], v[2] / 1e6, v[3] / 1e6, v[4]))
    for a in sorted(sel, key=lambda a: -psi[a.eventId])[:4]:
        ctrl.SetFrameEvent(a.eventId, False)
        ps = ctrl.GetPipelineState()
        texs = []
        try:
            for ro in ps.GetReadOnlyResources(rd.ShaderStage.Pixel)[:3]:
                d = ro.descriptor
                if d.resource != rd.ResourceId.Null():
                    texs.append(nm(d.resource)[:30])
        except Exception as e:
            texs.append('?' + str(e)[:40])
        print("    eid %5d ps %6.2f M passed %6.2f M idx %6d vs %s ps %s tex %s" % (
            a.eventId, psi[a.eventId] / 1e6, smp[a.eventId] / 1e6, a.numIndices,
            nm(ps.GetShader(rd.ShaderStage.Vertex))[-10:], nm(ps.GetShader(rd.ShaderStage.Pixel))[-10:], texs))
ctrl.Shutdown(); cap.Shutdown()
