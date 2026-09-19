"""Build R4 from reviewed R2 logic, with corrected rejection and actual C tests."""
import ast,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
s=(ROOT/'tools/Prepare-HapticBrakeFixR2.py').read_text()
tree=ast.parse(s)
node=next(n for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='new' for t in n.targets))
new=ast.literal_eval(node.value)
new=new.replace('const void *brake_prop;', 'const struct property *brake_prop;')
new=new.replace('of_get_property(', 'of_find_property(')
bad='ret = -EINVAL;\n\t\t\t\t\t\tbreak;'
assert new.count(bad)==2
new=new.replace(bad,'ret = -ERANGE;\n\t\t\t\t\t\tbreak;')
lines=s.splitlines(True)
lines[node.lineno-1:node.end_lineno]=['new='+repr(new)+'\n']
s=''.join(lines).replace('20260918-r2','20260918-r4')
s=s.replace('/home/a6l/kernel/out-a6l-peripheral-prep-20260917-r2','/home/a6l/kernel/out-a6l-peripheral-prep-20260917')
s=s.replace("BUILD.mkdir(exist_ok=True)","assert BUILD.is_dir()")
s=s.replace('M.mkdir(exist_ok=True)','M.mkdir(exist_ok=False)').replace('OUT.mkdir(exist_ok=True)','OUT.mkdir(exist_ok=False)')
if '--resume-test' in sys.argv:
    # Resume an offline fixture-only failure without changing the candidate C.
    saved=(ROOT/'firmware/extracted/haptics-brake-prep-20260918-r4/candidate.c').read_text()
    original=(ROOT/'firmware/extracted/haptics-brake-prep-20260918-r4/original.c').read_text()
    old_node=next(n for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='old' for t in n.targets))
    assert saved==original.replace(ast.literal_eval(old_node.value),new)
    s=s.replace(';M.mkdir(exist_ok=False)',';assert M.is_dir()').replace(';OUT.mkdir(exist_ok=False)',';assert OUT.is_dir()')
s=s.replace('androidboot.init_rc=/system/etc/init.rc','androidboot.init_rc=/system/etc/init/hw/init.rc')
start=s.index('parser_checks={');end=s.index("report={'built'",start)
s=s[:start]+"subprocess.run(['python3',str(ROOT/'tools/Test-HapticParserR4.py')],check=True)\nparser_checks=json.loads((OUT/'parser-behavior.json').read_text())\nassert parser_checks['passed']\n"+s[end:]
exec(compile(s,str(ROOT/'tools/Prepare-HapticBrakeFixR2.py'),'exec'),globals(),globals())
