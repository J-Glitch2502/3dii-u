#!/usr/bin/env python3
"""Does nothing, on purpose. This used to strip relocations before elf2rpl, and the console's loader refuses the result with error
160-2203, even for a tiny program (Cemu doesn't mind). Kept only because other build scripts still call it; delete it once they don't."""
