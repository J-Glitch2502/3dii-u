"""Compile the actual frontend mapping and right-stick selection without a console or game."""
from pathlib import Path
import re
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools' / 'win'))
import build3ds


def function(source, signature):
    start = source.index(signature)
    at = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[at] == '{') - (source[at] == '}')
        at += 1
    return source[start:at]


def main():
    source = (ROOT / 'port/wiiu/frontend_wiiu.cpp').read_text()
    mapped = '\n'.join(function(source, 'static u32 ' + name + '(')
                       for name in ('map_pro', 'map_buttons', 'pro_as_gamepad'))
    start = source.index('        float rx = pad_rx, ry = pad_ry;')
    selection = source[start:source.index('        cx = ', start)]
    constants = sorted(set(re.findall(r'(?:VPAD|WPAD_PRO)_(?:BUTTON|TRIGGER)_\w+', mapped + selection)))
    definitions = []
    for prefix in ('VPAD', 'WPAD_PRO'):
        values = [v for v in constants if v.startswith(prefix + '_BUTTON')]
        definitions.extend(f'constexpr u32 {v}=1u<<{i};' for i, v in enumerate(values))
    definitions += [f'constexpr u32 {v}={v.replace("TRIGGER", "BUTTON")};' for v in constants if '_TRIGGER_' in v]
    code = '#include <cassert>\n#include <cmath>\nusing u32=unsigned;\n' + '\n'.join(definitions) + '\n' + mapped
    code += '''
struct Input {float x,y; bool zl,zr,r;};
static Input select(float pad_rx,float pad_ry,u32 pad_held,const float pro_rx[4],const float pro_ry[4],const u32 pro_hold[4]) {
''' + selection + '''
return {rx,ry,zl,zr,r};
}
int main() {
    for(bool aliases : {false,true}) {
        assert(map_buttons(VPAD_BUTTON_L|VPAD_BUTTON_R, aliases)==(3u<<8));
        assert(map_pro(WPAD_PRO_BUTTON_L|WPAD_PRO_BUTTON_R, aliases)==(3u<<8));
        assert(map_buttons(VPAD_BUTTON_ZL|VPAD_BUTTON_ZR, aliases)==(aliases?(3u<<8):0));
        assert(map_pro(WPAD_PRO_BUTTON_ZL|WPAD_PRO_BUTTON_ZR, aliases)==(aliases?(3u<<8):0));
        assert(map_buttons(VPAD_BUTTON_A|VPAD_BUTTON_X, aliases)==(1u|(1u<<10)));
        assert(map_pro(WPAD_PRO_BUTTON_A|WPAD_PRO_BUTTON_X, aliases)==(1u|(1u<<10)));
    }
    assert(pro_as_gamepad(WPAD_PRO_TRIGGER_ZL|WPAD_PRO_TRIGGER_ZR|WPAD_PRO_BUTTON_STICK_L)==
           (VPAD_BUTTON_ZL|VPAD_BUTTON_ZR|VPAD_BUTTON_STICK_L));
    assert(pro_as_gamepad(WPAD_PRO_BUTTON_STICK_R|WPAD_PRO_BUTTON_MINUS)==
           (VPAD_BUTTON_STICK_R|VPAD_BUTTON_MINUS));
    float x[4]={0.1f,-0.8f,0.2f,0}, y[4]={0.1f,0.1f,0.1f,0};
    u32 h[4]={WPAD_PRO_BUTTON_ZR,WPAD_PRO_BUTTON_R,0,0};
    auto input=select(0.1f,0.1f,VPAD_BUTTON_ZL,x,y,h);
    assert(input.x==-0.8f && input.y==0.1f && input.zl && input.zr && input.r);
    input=select(0.9f,-0.9f,0,x,y,h);
    assert(input.x==0.9f && input.y==-0.9f && !input.zl && input.zr && input.r);
    for(int i=0;i<4;i++) {x[i]=y[i]=0;h[i]=0;}
    input=select(0,0,0,x,y,h);
    assert(input.x==0 && input.y==0 && !input.zl && !input.zr && !input.r);
}
'''
    code = '#include <initializer_list>\n' + code
    out = Path(build3ds.WORK) / 'selftest/ir_input'
    out.mkdir(parents=True, exist_ok=True)
    cpp, exe = out / 'input.cpp', out / 'input.exe'
    cpp.write_text(code)
    subprocess.run(build3ds.HOST_CXX + ['-std=c++20','-O1','-static',str(cpp),'-o',str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
    # Mapping is recalculated from held state each iteration, even without a fresh VPAD sample.
    thread = function(source, 'static int hid_thread(')
    assert 'pad_buttons' not in thread
    assert 'buttons = map_buttons(pad_held, z_too);' in thread
    assert 'pro_x[ch] = pro_y[ch] = pro_rx[ch] = pro_ry[ch] = 0;' in thread
    assert 'overlay_pressed(st.trigger, st.hold)' in thread
    assert 'overlay_pressed(down, pro_as_gamepad(pro_hold[ch]))' in thread
    assert 'menu_took_input = menu_was_open || wiiu::menu_open();' in thread
    assert re.search(r'if \(menu_took_input\) \{[^}]*kernel::hid_extra_pad\(0, 0, false, false, false\);', thread)
    print('IR input: actual GamePad/Pro mappings, menu chord, strongest right stick and trigger merging passed')


if __name__ == '__main__':
    main()
