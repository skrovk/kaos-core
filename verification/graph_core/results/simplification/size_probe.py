from pathlib import Path
import json, shlex, subprocess
root=Path('/home/karolina/Research/Code/kaos_core')
work=Path('/tmp/kaos-simplification-20261022')
probe=work/'sizes.c'
probe.write_text('''#include "kaos_host.h"
#include "host_udp.h"
unsigned char size_host[sizeof(core_host)];
unsigned char size_admission[sizeof(core_admission)];
unsigned char size_control[sizeof(core_control)];
unsigned char size_node[sizeof(core_node_service)];
unsigned char size_endpoint[sizeof(core_endpoint_service)];
unsigned char size_edge[sizeof(core_edge_service)];
unsigned char size_staging[sizeof(core_host_staging)];
unsigned char size_udp[sizeof(core_host_udp)];
''')
commands=json.loads(Path('/tmp/kaos-transport-esp32/compile_commands.json').read_text())
row=next(r for r in commands if r['file'].endswith('/kaos_admission.c'))
records={}
for target in ['native','esp32']:
    for profile in ['default','four']:
        for version in ['before','after']:
            directory=work/'sizes'/target/profile/version; directory.mkdir(parents=True,exist_ok=True)
            output=directory/'sizes.o'
            if target=='esp32':
                args=shlex.split(row['command'])
                args[args.index('-o')+1]=str(output)
                args[args.index('-c')+1]=str(probe)
                if profile=='default': args=[x for x in args if x!='-DCORE_FOUR_NODE_PROFILE=1']
                nm=args[0].replace('gcc','nm')
            else:
                args=['cc','-I'+str(root/'graph_core/include'),'-c',str(probe),'-o',str(output)]
                if profile=='four': args+=['-DCORE_FOUR_NODE_PROFILE=1']
                nm='nm'
            if version=='before':
                args=[x.replace(str(root/'graph_core'),str(work/'before/graph_core')) for x in args]
                args+=['-I'+str(work/'before/graph_core/ports')]
            subprocess.run(args,cwd=row['directory'],check=True,capture_output=True)
            sizes={}
            for line in subprocess.check_output([nm,'-S','--defined-only',str(output)],text=True).splitlines():
                fields=line.split()
                if len(fields)==4 and fields[3].startswith('size_'): sizes[fields[3][5:]]=int(fields[1],16)
            records[f'{target}/{profile}/{version}']=sizes
for target in ['native','esp32']:
    for profile in ['default','four']:
        assert records[f'{target}/{profile}/before']==records[f'{target}/{profile}/after']
        print(target,profile,records[f'{target}/{profile}/after'])
(work/'sizes.json').write_text(json.dumps(records,indent=2)+'\n')
