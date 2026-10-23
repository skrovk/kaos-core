from pathlib import Path
import json, shlex, subprocess
root=Path('/home/karolina/Research/Code/kaos_core')
work=Path('/tmp/kaos-simplification-20261022')
commands=json.loads(Path('/tmp/kaos-transport-esp32/compile_commands.json').read_text())
names={'kaos_admission.c','kaos_request.c','kaos_control.c','kaos_artifact.c','kaos_endpoint.c','kaos_edge.c','kaos_node.c','kaos_host.c','host_udp.c','kaos_wamr.c'}
records={}
for row in commands:
    source=Path(row['file'])
    if source.name not in names or '/graph_core/' not in str(source): continue
    relative=source.relative_to(root)
    for profile in ['default','four']:
        for version in ['before','after']:
            directory=work/'stack'/profile/version
            directory.mkdir(parents=True,exist_ok=True)
            args=shlex.split(row['command'])
            output=directory/(source.stem+'.o')
            args[args.index('-o')+1]=str(output)
            if profile=='default': args=[x for x in args if x!='-DCORE_FOUR_NODE_PROFILE=1']
            if version=='before':
                args=[x.replace(str(root/'graph_core'),str(work/'before/graph_core')) for x in args]
                if source.name=='host_udp.c':
                    args=[str(work/'before/graph_core/ports/host_udp.c') if x==str(work/'before/graph_core/host_udp.c') else x for x in args]
            elif source.name=='host_udp.c':
                args=[str(root/'graph_core/host_udp.c') if x==str(source) else x for x in args]
            args+=['-fstack-usage']
            proc=subprocess.run(args,cwd=row['directory'],capture_output=True,text=True)
            (directory/(source.stem+'.log')).write_text(proc.stdout+proc.stderr)
            if proc.returncode: raise SystemExit(f'{profile}/{version}/{source.name}: {proc.stderr}')
            sizes={}
            for line in output.with_suffix('.su').read_text().splitlines():
                origin,size,kind=line.split('\t')
                sizes[origin.rsplit(':',1)[-1]]=int(size)
            records[f'{profile}/{version}/{source.name}']=sizes
(work/'stack.json').write_text(json.dumps(records,indent=2)+'\n')
for key,value in records.items():
    if '/before/' not in key: continue
    after=records[key.replace('/before/','/after/')]
    changed={name:(size,after[name]) for name,size in value.items() if name in after and size!=after[name]}
    added={name:size for name,size in after.items() if name not in value}
    if changed or added: print(key.replace('/before/','/'), 'changed:',changed,'new:',added)
