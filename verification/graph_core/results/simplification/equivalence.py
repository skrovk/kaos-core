from pathlib import Path
import subprocess
root=Path('/home/karolina/Research/Code/kaos_core')
work=Path('/tmp/kaos-simplification-20261022')
before=work/'before/graph_core'
sources=[before/'kaos_admission.c',before/'kaos_request.c',before/'ports/host_udp.c']
for profile in ['default','four']:
    d=work/'differential'/profile; d.mkdir(parents=True,exist_ok=True)
    flags=['-std=c11','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-Wall','-Wextra','-Werror','-Wno-error=unused-result']
    if profile=='four': flags+=['-DCORE_FOUR_NODE_PROFILE=1']
    def run(cmd): subprocess.run(cmd,check=True)
    objects=[]; symbols=[]
    for source in sources:
        obj=d/(source.stem+'.o'); objects.append(obj)
        run(['cc',*flags,'-I'+str(before/'include'),'-c',str(source),'-o',str(obj)])
        for line in subprocess.check_output(['nm','-g','--defined-only',str(obj)],text=True).splitlines():
            fields=line.split()
            if len(fields)==3 and fields[1]=='T': symbols.append(fields[2])
    macros=['-D'+name+'=before_'+name for name in symbols]
    for source,obj in zip(sources,objects):
        run(['cc',*flags,*macros,'-I'+str(before/'include'),'-c',str(source),'-o',str(obj)])
    build='/tmp/kaos-timing-host' if profile=='default' else '/tmp/kaos-refactor-four'
    run(['cc',*flags,'-I'+str(root/'graph_core/include'),'-I'+str(root/'verification/graph_core/tests'),str(work/'equivalence.c'),str(root/'graph_core/host_udp.c'),*[str(x) for x in objects],build+'/core/libkaos_graph_core.a','-pthread','-o',str(d/'compare')])
    print('Built:',d/'compare')
