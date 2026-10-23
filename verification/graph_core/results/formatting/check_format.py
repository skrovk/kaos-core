from pathlib import Path
import re,subprocess,difflib,json,hashlib
root=Path('/home/karolina/Research/Code/kaos_core'); work=Path('/tmp/kaos-semantic-format-20261022')
files=sorted(p for p in (root/'graph_core').rglob('*') if p.suffix in ('.c','.h'))
subprocess.run(['/opt/wasi-sdk/bin/clang-format','--dry-run','--Werror',*[str(p) for p in files]],check=True)
lex=re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/|\\\n|\s+|[A-Za-z_][A-Za-z_0-9]*|[0-9]+|>>=|<<=|\.\.\.|\+\+|--|->|&&|\|\||==|!=|<=|>=|<<|>>|[+*/%&|^!-]=|##|.',re.S)
def tokens(s): return [t for t in lex.findall(s) if not t.isspace() and t!='\\\n' and not t.startswith(('//','/*'))]
semantic=[]; mechanical=[]; patches=[]; functions={}; hashes={}
for p in files:
 rel=p.relative_to(root/'graph_core'); before=(work/'before/graph_core'/rel).read_text(); fmt=(work/'formatted'/rel).read_text(); after=p.read_text()
 hashes[str(p.relative_to(root))]=hashlib.sha256(p.read_bytes()).hexdigest()
 if tokens(before)!=tokens(fmt): mechanical.append(str(rel))
 if tokens(fmt)!=tokens(after):
  semantic.append(str(rel)); functions[str(rel)]=[]
  for m in re.finditer(r'(?m)^[\w *]+\b(\w+)\([^;]*?\)\n\{',fmt):
   name=m.group(1); end=fmt.index('\n}',m.end())+2
   n=re.search(r'(?m)^[\w *]+\b'+name+r'\([^;]*?\)\n\{',after); assert n
   if tokens(fmt[m.start():end])!=tokens(after[n.start():after.index('\n}',n.end())+2]): functions[str(rel)].append(name)
 patches.extend(difflib.unified_diff(fmt.splitlines(True),after.splitlines(True),fromfile='formatted/'+str(rel),tofile='after/'+str(rel)))
(work/'semantic.patch').write_text(''.join(patches))
record={'formatter':'clang-format 14.0.4','checked_files':len(files),'mechanical_token_changes':mechanical,'control_flow_edited_functions':functions}
(work/'token-check.json').write_text(json.dumps(record,indent=2)+'\n')
(work/'source-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
assert not mechanical, mechanical
assert set(semantic)=={'kaos_admission.c','kaos_node.c','kaos_endpoint.c','kaos_coap.c','kaos_host.c'}, semantic
baseline=json.loads((work/'baseline.json').read_text()); tests={p:h for p,h in baseline.items() if p.startswith('verification/')}
assert all(hashlib.sha256((root/p).read_bytes()).hexdigest()==h for p,h in tests.items())
(work/'preserved-test-hashes.json').write_text(json.dumps(tests,indent=2)+'\n')
print(json.dumps(record,indent=2)); print('Preserved existing verification files:',len(tests))
