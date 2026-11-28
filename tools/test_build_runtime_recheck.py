"""The runtime is rechecked after a long game-code compilation, before linking."""
from pathlib import Path
from types import SimpleNamespace
source=(Path(__file__).resolve().parent/'win/build3ds.py').read_text()
a=source.index('def build_code(g, args):');b=source.index('\n\ndef ',a+1)
state=dict(version=1,compiled=0,checks=[],generated=False,linked=False)
def runtime(g,args):
 state['checks'].append(state['version']);state['compiled']=state['version'];return 'runtime'
def code(g,args):
 state['version']=2 # represents an edit completed while game files compile
 return 'game-code'
def link(g,rt,objects,flags):
 assert state['compiled']==state['version']==2
 assert (rt,objects,flags)==('runtime','game-code',None)
 state['linked']=True
ns=dict(need_gen=lambda g:False,link_setup=lambda:None,build_runtime=runtime,build_game_code=code,link_game=link)
exec(source[a:b],ns)
ns['build_code'](object(),SimpleNamespace())
assert state['checks']==[1,2] and state['linked']
print('PASS: runtime edits during game compilation are rebuilt before link')
