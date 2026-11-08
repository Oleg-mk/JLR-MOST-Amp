"""Production package for one board folder: Gerbers + Excellon (zip for upload), BOM and CPL in JLCPCB's columns,
schematic PDF, assembly drawings, drill map. Run with KiCad's python (kicad-cli sits next to it):
    python.exe fab.py ../../hw 2L        -> hw/production/
    python.exe fab.py ../../hw4 4L       -> hw4/production/
    python.exe fab.py ../../tube/6n28b tube-6N28B   -> tube/6n28b/production/
    python.exe fab.py ../../tube/6n16b tube-6N16B ../../tube/6n28b/jlr_tube_6n28b.kicad_pcb   -> tube/6n16b/production/ (its schematic and BOM, the shared board)
The board's zones are refilled in memory if needed (--check-zones); the board file itself is not written."""
import csv, glob, os, shutil, subprocess, sys, zipfile

src, tag = os.path.abspath(sys.argv[1]), sys.argv[2]
cli = os.path.join(os.path.dirname(sys.executable), 'kicad-cli.exe')
base = os.path.basename(glob.glob(os.path.join(src, '*.kicad_pro'))[0])[:-len('.kicad_pro')]
pcb = os.path.abspath(sys.argv[3]) if len(sys.argv) > 3 else os.path.join(src, base + '.kicad_pcb')
sch = os.path.join(src, base + '.kicad_sch')
stem = os.path.splitext(os.path.basename(pcb))[0]
out = os.path.join(src, 'production'); ger = os.path.join(out, 'gerbers')
name = 'Bitline-' + tag
sch_pdf = 'Bitline-schematic.pdf' if tag in ('2L', '4L') else name + '-schematic.pdf'
os.makedirs(out, exist_ok=True)
for f in os.listdir(out):                                        # everything is regenerated except the README
    if f != 'README.md':
        p = os.path.join(out, f); shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
os.makedirs(ger)

def run(*args):
    r = subprocess.run([cli, *args], capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.returncode: sys.exit('kicad-cli failed: %s\n%s%s' % (' '.join(args), r.stdout, r.stderr))

cu = ['F.Cu'] + (['In1.Cu', 'In2.Cu'] if tag.endswith('4L') else []) + ['B.Cu']
layers = cu + ['F.Paste', 'B.Paste', 'F.Silkscreen', 'B.Silkscreen', 'F.Mask', 'B.Mask', 'Edge.Cuts']
run('pcb', 'export', 'gerbers', '-o', ger, '-l', ','.join(layers), '--no-x2', '--no-netlist',
    '--subtract-soldermask', '--precision', '6', '--check-zones', pcb)
run('pcb', 'export', 'drill', '-o', ger, '--format', 'excellon', '--excellon-units', 'mm',
    '--excellon-zeros-format', 'decimal', '--excellon-oval-format', 'alternate', '--excellon-separate-th',
    '--generate-map', '--map-format', 'pdf', pcb)
for m in glob.glob(os.path.join(ger, '*.pdf')):                 # drill maps are for people, not for the upload
    shutil.move(m, os.path.join(out, name + '-' + os.path.basename(m).replace(stem + '-', '')))
with zipfile.ZipFile(os.path.join(out, name + '-gerbers.zip'), 'w', zipfile.ZIP_DEFLATED) as z:
    for f in sorted(os.listdir(ger)): z.write(os.path.join(ger, f), f)

# BOM grouped by value + footprint, references without ranges (JLC reads "C1,C2"), library prefix dropped
tmp = os.path.join(out, '_bom.csv')
run('sch', 'export', 'bom', '-o', tmp, '--fields', 'Value,Reference,Footprint,${QUANTITY},LCSC',
    '--labels', 'Comment,Designator,Footprint,Quantity,LCSC Part #', '--group-by', 'Value,Footprint',
    '--ref-range-delimiter', '', '--exclude-dnp', sch)
rows = list(csv.reader(open(tmp, encoding='utf-8-sig')))
for r_ in rows[1:]: r_[2] = r_[2].split(':', 1)[-1]
csv.writer(open(os.path.join(out, name + '-BOM.csv'), 'w', newline='', encoding='utf-8')).writerows(rows)
refs = {x for r_ in rows[1:] for x in r_[1].split(',')}
os.remove(tmp)

# CPL: KiCad's absolute positions (same origin as the Gerbers), JLC's column names, only parts that are in the BOM
tmp = os.path.join(out, '_pos.csv')
run('pcb', 'export', 'pos', '-o', tmp, '--format', 'csv', '--units', 'mm', '--side', 'both', '--exclude-dnp', pcb)
pos = list(csv.DictReader(open(tmp, encoding='utf-8-sig')))
with open(os.path.join(out, name + '-CPL.csv'), 'w', newline='', encoding='utf-8') as f:
    w = csv.writer(f); w.writerow(['Designator', 'Mid X', 'Mid Y', 'Layer', 'Rotation'])
    for p in sorted(pos, key=lambda p: (p['Ref'].rstrip('0123456789'), int(''.join(c for c in p['Ref'] if c.isdigit()) or 0))):
        if p['Ref'] in refs:
            w.writerow([p['Ref'], p['PosX'] + 'mm', p['PosY'] + 'mm', p['Side'].capitalize(), p['Rot']])
missing = sorted(refs - {p['Ref'] for p in pos})
os.remove(tmp)

run('sch', 'export', 'pdf', '-o', os.path.join(out, sch_pdf), sch)
run('pcb', 'export', 'pdf', '--mode-single', '--sp', '--cdnp', '-l', 'F.Fab,F.Silkscreen,Edge.Cuts',
    '-o', os.path.join(out, name + '-assembly-top.pdf'), pcb)
run('pcb', 'export', 'pdf', '--mode-single', '--sp', '--cdnp', '--mirror', '-l', 'B.Fab,B.Silkscreen,Edge.Cuts',
    '-o', os.path.join(out, name + '-assembly-bottom.pdf'), pcb)

print(out)
for f in sorted(os.listdir(out)): print('  ', f)
print('   gerbers/:', ' '.join(sorted(os.listdir(ger))))
print('BOM lines %d, parts %d; CPL parts %d; in BOM but not on the board: %s'
      % (len(rows) - 1, len(refs), len(refs) - len(missing), missing or 'none'))
