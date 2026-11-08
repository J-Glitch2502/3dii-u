#!/bin/bash
# Runs inside the contained environment: writes /root/rpx.ld and /root/rpx.specs, wut's linker files adjusted for very large code.
# The C runtime's .init and .fini are pieces of one function that fall through into each other. With wut's script they land
# in .text with their callees at the far end of it, and ld --relax then places branch stubs in the fall-through path.
# Keeping them, and crtbegin/crtend's code, together at the start of .text means no far branch is ever needed there.
W=/opt/devkitpro/wut/share
python3 - "$W/wut.ld" /root/rpx.ld <<'EOF'
import sys
s = open(sys.argv[1]).read()
init = '      KEEP (*(SORT_NONE(.init)))\n'
fini = '      KEEP (*(SORT_NONE(.fini)))\n'
assert init in s and fini in s
s = s.replace(fini, '')
s = s.replace(init, init + fini + '      *crtbegin*.o(.text .text.*)\n      *crtend*.o(.text .text.*)\n')
open(sys.argv[2], 'w').write(s)
EOF
sed 's#-T %:getenv(DEVKITPRO /wut/share/wut.ld)#-T /root/rpx.ld#' "$W/wut.specs" > /root/rpx.specs
grep -c '/root/rpx.ld' /root/rpx.specs
