import os, sys, collections
RD = r'C:\Users\rieng\Documents\GitHub\renderdoc\x64\Development'
os.add_dll_directory(RD)
sys.path.insert(0, os.path.join(RD, 'pymodules'))
import renderdoc as rd

path = sys.argv[1]
topn = int(sys.argv[2]) if len(sys.argv) > 2 else 40
rd.InitialiseReplay(rd.GlobalEnvironment(), [])
cap = rd.OpenCaptureFile()
assert cap.OpenFile(path, '', None) == rd.ResultCode.Succeeded
st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
assert st == rd.ResultCode.Succeeded
sf = ctrl.GetStructuredFile()
acts = []
def flat(a):
    for x in a:
        acts.append(x); flat(x.children)
flat(ctrl.GetRootActions())
names = {r.resourceId: r.name for r in ctrl.GetResources()}
nm = lambda rid: names.get(rid, str(rid))
texinfo = {t.resourceId: t for t in ctrl.GetTextures()}
def desc(rid):
    t = texinfo.get(rid)
    if not t:
        return nm(rid)
    return "%s %dx%d s%d %s" % (nm(rid)[:26], t.width, t.height, t.msSamp, str(t.format.Name())[:12])

res = ctrl.FetchCounters([rd.GPUCounter.EventGPUDuration])
dur = collections.defaultdict(float)
for r in res:
    dur[r.eventId] += r.value.d * 1000.0

def kind(a):
    f = a.flags
    if f & rd.ActionFlags.Drawcall: return 'draw'
    if f & rd.ActionFlags.Resolve: return 'resolve'
    if f & rd.ActionFlags.Copy: return 'copy'
    if f & rd.ActionFlags.Clear: return 'clear'
    if f & rd.ActionFlags.Dispatch: return 'dispatch'
    if f & rd.ActionFlags.Present: return 'present'
    return 'other'

bykind = collections.defaultdict(lambda: [0, 0.0])
for a in acts:
    k = kind(a)
    bykind[k][0] += 1; bykind[k][1] += dur[a.eventId]
total = sum(dur.values())
print("events %d, GPU sum %.2f ms" % (len(acts), total))
for k, v in sorted(bykind.items(), key=lambda kv: -kv[1][1]):
    print("  %-9s %5d events %7.2f ms" % (k, v[0], v[1]))

xfers = [a for a in acts if kind(a) in ('resolve', 'copy', 'clear', 'dispatch')]
print("\n--- top %d transfers ---" % topn)
for a in sorted(xfers, key=lambda a: -dur[a.eventId])[:topn]:
    src = desc(a.copySource) if a.copySource != rd.ResourceId.Null() else '-'
    dst = desc(a.copyDestination) if a.copyDestination != rd.ResourceId.Null() else '-'
    print("eid %5d %-8s %6.3f ms  %s -> %s  | %s" % (a.eventId, kind(a), dur[a.eventId], src, dst,
                                                   a.GetName(sf)[:60]))

bydst = collections.defaultdict(lambda: [0, 0.0])
for a in xfers:
    d = a.copyDestination if a.copyDestination != rd.ResourceId.Null() else rd.ResourceId.Null()
    bydst[(kind(a), desc(d))][0] += 1; bydst[(kind(a), desc(d))][1] += dur[a.eventId]
print("\n--- transfers by kind and destination ---")
for k, v in sorted(bydst.items(), key=lambda kv: -kv[1][1])[:30]:
    print("  %-8s %-52s x%3d %7.3f ms" % (k[0], k[1], v[0], v[1]))
ctrl.Shutdown(); cap.Shutdown()
