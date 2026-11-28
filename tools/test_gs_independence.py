"""Conservative loop/early-return proofs for native geometry invocations."""
from pathlib import Path
import struct,subprocess,sys,tempfile
import pica_to_c as p
IDENTITY=15|(0x1b<<5)|(0x1b<<14)|(0x1b<<23)
END=0x22<<26
NOP=0x21<<26
def mov(dst,src):return (0x13<<26)|(dst<<21)|(src<<12)
def emit(vid,prim=False):return [(0x2b<<26)|(vid<<24)|(int(prim)<<23),0x2a<<26]
def triangle():
 a=[]
 for i in range(3):a += [mov(0,0x20)]+emit(i,i==2)
 return a
body=triangle()
loop=[(0x29<<26)|(len(body)<<10)]+body+[END]
returned=[(0x2d<<26)|(14<<22)|((len(loop)+1-1)<<10)]+loop
# Fix LOOP's absolute end after the return prefix.
returned[1]=(0x29<<26)|((1+len(body))<<10)
valid=[triangle()+[END],loop,returned]
for c in valid:
 _,end,_=p.translate(0,c,[IDENTITY],'test')
 assert p.independence(0,end,c,[IDENTITY])==(True,1)
# A primitive that consumes two emitter slots left over by an older invocation.
prior= [mov(0,0x20)]+emit(2,True)+[END]
assert not p.independence(0,len(prior),prior,[IDENTITY])[0]
# b15 changes after invocation one, and initial comparison flags are not owned.
for prefix in [(0x2d<<26)|(15<<22), (0x2c<<26)]:
 c=[prefix|((len(body)+1)<<10)]+body+[END]
 assert not p.independence(0,len(c),c,[IDENTITY])[0]
# Loop reads an unwritten scratch lane; later writes must not justify it.
c=[(0x29<<26)|(2<<10),mov(0,0x10),mov(0x10,0x20)]+triangle()+[END]
assert not p.independence(0,len(c),c,[IDENTITY])[0]
with tempfile.TemporaryDirectory(prefix='gs-loop-proof-') as tmp:
 f=Path(tmp)/'synthetic.gsl'
 f.write_bytes(b''.join(b'GS  '+struct.pack('<III',0,len(c),1)+struct.pack('<%dI'%len(c),*c)+struct.pack('<I',IDENTITY) for c in valid))
 subprocess.run([sys.executable,str(Path(__file__).with_name('test_gs_native.py')),tmp],check=True)
print('PASS: bounded loop parity, returns, prior-condition/b15 rejection, and emitter/scratch ownership')
