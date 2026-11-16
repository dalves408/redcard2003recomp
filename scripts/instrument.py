"""Apply optional boot diagnostics to the generated C (no guest-state changes)."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
path = root / 'src/recomp/gen/recomp_0013.c'
text = path.read_text(encoding='utf-8')
for address, hook in [('000F18BF', 'redcard_trace_event'), ('000F1B4C', 'redcard_trace_frame')]:
    original = f'void sub_{address}(void)\n{{\n'
    replacement = original + f'    extern void {hook}(void);\n    {hook}();\n'
    if replacement not in text:
        if original not in text:
            raise RuntimeError(f'Function {address} not found')
        text = text.replace(original, replacement, 1)
path.write_text(text, encoding='utf-8')
text = path.read_text(encoding='utf-8')
original = 'void sub_000FDE20(void)\n{\n'
replacement = original + '    extern int redcard_skip_logo(void);\n    if (redcard_skip_logo()) return;\n'
if replacement not in text:
    if original not in text: raise RuntimeError('Movie entry not found')
    text = text.replace(original, replacement, 1)
path.write_text(text, encoding='utf-8')
