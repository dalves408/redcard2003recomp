"""Run the pinned xboxrecomp analysis pipeline and optionally lift to C."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
toolkit = root / 'tools/xboxrecomp'
xbe = root / 'game_files/default.xbe'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--lift', action='store_true', help='Generate C after analysis')
parser.add_argument('--after-disasm', action='store_true', help='Reuse existing disassembly')
args = parser.parse_args()
env = os.environ.copy()
env['PYTHONPATH'] = str(root / '.deps') + os.pathsep + env.get('PYTHONPATH', '')
env['PYTHONIOENCODING'] = 'utf-8'
(root / 'analysis').mkdir(exist_ok=True)

def run(module, *options):
    log = root / 'analysis' / (module.rsplit('.', 1)[-1] + '.log')
    print(f'Running {module}; log: {log}', flush=True)
    with log.open('w', encoding='utf-8') as output:
        subprocess.run([sys.executable, '-m', module, str(xbe), *map(str, options)],
                       cwd=toolkit, env=env, stdout=output, stderr=subprocess.STDOUT, check=True)

if not args.after_disasm:
    run('tools.xbe_parser', '--json', xbe.with_name('default_analysis.json'))
    run('tools.disasm', '--text-only', '--extra-sections', 'D3D,D3DX,XGRPH,DSOUND,XPP,DOLBY')
run('tools.func_id', '-v')
run('tools.abi_analysis', '-v')
if args.lift:
    run('tools.recomp', '--all', '--split', '250', '--game-name', 'RedCard 20-03',
        '--gen-dir', root / 'src/recomp/gen')
    subprocess.run([sys.executable, str(root / 'scripts/patch_fibers.py')],
                   cwd=root, env=env, check=True)
    subprocess.run([sys.executable, str(root / 'scripts/patch_input.py')],
                   cwd=root, env=env, check=True)
    subprocess.run([sys.executable, str(root / 'scripts/instrument.py')],
                   cwd=root, env=env, check=True)
print('Pipeline complete.', flush=True)
