"""Route the verified XDK GetState entry through optional diagnostics."""
from pathlib import Path
p = Path(__file__).resolve().parents[1] / 'src/recomp/gen/recomp_0040.c'
s = p.read_text(encoding='utf-8')
old, new = 'void sub_0022A1DB(void)', 'void redcard_original_input_state(void)'
if new not in s:
    if s.count(old) != 1: raise RuntimeError('GetState definition missing')
    p.write_text(s.replace(old, new, 1), encoding='utf-8')
