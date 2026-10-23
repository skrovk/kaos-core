from pathlib import Path
import subprocess,shlex,json,hashlib
root=Path('/home/karolina/Research/Code/kaos_core'); work=Path('/tmp/kaos-semantic-format-20261022')
results=[]
for profile,build in [('default',Path('/tmp/kaos-timing-host')),('four',Path('/tmp/kaos-refactor-four'))]:
 lines=subprocess.check_output(['ninja','-C',str(build),'-t','commands','kaos_host'],text=True).splitlines()
 seen=set()
 for line in lines:
  args=shlex.split(line)
  if '-c' not in args: continue
  source=Path(args[args.index('-c')+1])
  if not source.is_relative_to(root/'graph_core'): continue
  rel=source.relative_to(root)
  if str(rel)=='graph_core/ports/clock_posix.c': rel=Path('graph_core/ports/posix/clock.c')
  if rel in seen: continue
  seen.add(rel)
  flags=[]; skip=False
  for arg in args[1:]:
   if skip: skip=False; continue
   if arg in ['-c','-o','-MT','-MF']: skip=True; continue
   if arg in ['-MD','-MMD','-fstack-usage'] or arg.startswith(('-fsanitize','-fno-sanitize','-O','-g')): continue
   flags.append(arg)
  flags+=['-O2','-g0','-fno-ident','-frandom-seed=kaos-format','-Wno-error=unused-result']
  paths=[]
  for version in ['before','after']:
   base=work/'before' if version=='before' else root
   out=work/'codegen'/profile/version/rel.with_suffix('.o'); out.parent.mkdir(parents=True,exist_ok=True)
   selected=[x.replace(str(root/'graph_core'),str(base/'graph_core')) for x in flags]
   selected+=['-ffile-prefix-map='+str(base)+'=kaos-source']
   # Reading the translation unit from stdin avoids differing .file paths in ELF.
   selected+=['-I'+str((base/rel).parent),'-x','c','-c','-','-o',str(out)]
   proc=subprocess.run([args[0],*selected],input=(base/rel).read_bytes(),cwd=build,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
   out.with_suffix('.log').write_bytes(proc.stdout+proc.stderr)
   if proc.returncode: raise SystemExit(f'{profile}/{version}/{rel}: '+proc.stderr.decode())
   paths.append(out)
  equal=paths[0].read_bytes()==paths[1].read_bytes()
  results.append({'profile':profile,'source':str(rel),'equal':equal,'sha256':[hashlib.sha256(p.read_bytes()).hexdigest() for p in paths]})
  print(profile,rel,'IDENTICAL' if equal else 'DIFFERS',flush=True)
(work/'codegen.json').write_text(json.dumps(results,indent=2)+'\n')
print('Identical objects:',sum(r['equal'] for r in results),'/',len(results))
