"""Route RedCard's XAPI fiber functions through the native fiber bridge."""
from pathlib import Path

generated = Path(__file__).resolve().parents[1] / "src/recomp/gen/recomp_0030.c"
text = generated.read_text(encoding="utf-8")
functions = {
    "001D009A": "CreateFiber",
    "001D0126": "DeleteFiber",
    "001D0139": "SwitchToFiber",
    "001D017F": "ConvertThreadToFiber",
}

for address, name in functions.items():
    original = f"void sub_{address}(void)"
    replacement = f"void redcard_original_{name}(void)"
    if replacement in text:
        continue
    if text.count(original) != 1:
        raise RuntimeError(f"Expected exactly one definition for sub_{address}")
    text = text.replace(original, replacement, 1)

generated.write_text(text, encoding="utf-8")
