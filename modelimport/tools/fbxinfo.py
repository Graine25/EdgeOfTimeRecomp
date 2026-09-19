import sys, os, collections
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fbxparse import parse

v, root = parse(sys.argv[1])
objects = root.first('Objects')
conns = root.first('Connections')
by_id = {}
kinds = collections.Counter()
for o in objects.children:
    oid = o.props[0] if o.props else None
    by_id[oid] = o
    kinds[o.name + ':' + (o.props[2] if len(o.props) > 2 and isinstance(o.props[2], str) else '')] += 1
print('FBX', v, 'objects:', dict(kinds))

parents = collections.defaultdict(list); children = collections.defaultdict(list)
for c in conns.children:
    if c.name != 'C':
        continue
    ctype, a, b = c.props[0], c.props[1], c.props[2]
    prop = c.props[3] if len(c.props) > 3 else None
    parents[a].append((b, ctype, prop)); children[b].append((a, ctype, prop))

def name_of(o):
    return o.props[1].split('\x00')[0] if len(o.props) > 1 and isinstance(o.props[1], str) else '?'

print('\n== Models ==')
for o in objects.children:
    if o.name != 'Model':
        continue
    kind = o.props[2]
    par = [by_id.get(p[0]) for p in parents.get(o.props[0], [])]
    pnames = [name_of(p) for p in par if p is not None and p.name == 'Model']
    props70 = o.first('Properties70')
    lt = lr = ls = None
    if props70:
        for p in props70.children:
            if p.props[0] == 'Lcl Translation': lt = tuple(round(x, 3) for x in p.props[4:7])
            if p.props[0] == 'Lcl Rotation': lr = tuple(round(x, 3) for x in p.props[4:7])
            if p.props[0] == 'Lcl Scaling': ls = tuple(round(x, 3) for x in p.props[4:7])
    if kind in ('Mesh',) or len([1 for _ in objects.children]) < 400:
        print('  %-10s %-40s parent=%s T=%s R=%s S=%s' % (kind, name_of(o), pnames, lt, lr, ls))

print('\n== Geometries ==')
for o in objects.children:
    if o.name != 'Geometry':
        continue
    verts = o.first('Vertices').props[0] if o.first('Vertices') else np.zeros(0)
    idx = o.first('PolygonVertexIndex').props[0] if o.first('PolygonVertexIndex') else np.zeros(0)
    npoly = int((idx < 0).sum())
    layers = [c.name for c in o.children if c.name.startswith('LayerElement')]
    mat = o.first('LayerElementMaterial')
    matmode = mat.first('MappingInformationType').props[0] if mat else None
    uvs = o.find('LayerElementUV')
    owner = [name_of(by_id[p[0]]) for p in parents.get(o.props[0], []) if p[0] in by_id]
    print('  geom %-36s verts %6d polys %6d layers %s matmap=%s uvsets=%d owner=%s' % (
        name_of(o), len(verts) // 3, npoly, [l.replace('LayerElement', '') for l in layers], matmode, len(uvs), owner))
    deformers = [by_id[c[0]] for c in children.get(o.props[0], []) if c[0] in by_id and by_id[c[0]].name == 'Deformer']
    for df in deformers:
        clusters = [by_id[c[0]] for c in children.get(df.props[0], []) if c[0] in by_id and by_id[c[0]].name == 'Deformer']
        print('     skin %s: %d clusters' % (name_of(df), len(clusters)))
        tot = 0
        for cl in clusters[:200]:
            ind = cl.first('Indexes'); w = cl.first('Weights')
            bone = [name_of(by_id[c[0]]) for c in children.get(cl.props[0], []) if c[0] in by_id and by_id[c[0]].name == 'Model']
            n = len(ind.props[0]) if ind else 0
            tot += n
        print('     total weight entries %d' % tot)
        names = []
        for cl in clusters:
            bone = [name_of(by_id[c[0]]) for c in children.get(cl.props[0], []) if c[0] in by_id and by_id[c[0]].name == 'Model']
            ind = cl.first('Indexes')
            names.append('%s(%d)' % (bone[0] if bone else '?', len(ind.props[0]) if ind else 0))
        print('     bones:', ', '.join(names))

print('\n== Materials / Textures ==')
for o in objects.children:
    if o.name in ('Material', 'Texture', 'Video'):
        extra = ''
        if o.name == 'Texture':
            fn = o.first('RelativeFilename') or o.first('FileName')
            extra = fn.props[0] if fn else ''
            owners = [name_of(by_id[p[0]]) + ':' + str(p[2]) for p in parents.get(o.props[0], []) if p[0] in by_id]
            extra += '  -> ' + str(owners)
        if o.name == 'Material':
            users = [name_of(by_id[p[0]]) for p in parents.get(o.props[0], []) if p[0] in by_id]
            extra = 'used by ' + str(users)
        print('  %-8s %-40s %s' % (o.name, name_of(o), extra))
