"""Launch RedCard from its asset root with native stdout/stderr log handles."""
import argparse
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--seconds', type=int, default=0, help='Optional watchdog timeout')
parser.add_argument('--label', default='game')
parser.add_argument('--configuration', choices=['Debug','Release','RelWithDebInfo'], default='Debug')
args = parser.parse_args()
if not args.label or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for c in args.label):
    parser.error('label must contain only letters, numbers, hyphens or underscores')
env = os.environ.copy()
if args.seconds:
    env['RECOMP_WATCHDOG_SECS'] = str(args.seconds)
else:
    env.pop('RECOMP_WATCHDOG_SECS', None)
env.setdefault('RECOMP_PB_REPORT_MS', '3000')
env.setdefault('RECOMP_KERNEL_LOG_BUDGET', '50')
logs = root / 'analysis'
with (logs / f'{args.label}.stdout.log').open('wb') as stdout, (logs / f'{args.label}.stderr.log').open('wb') as stderr:
    process = subprocess.Popen([str(root / 'build' / args.configuration / 'redcard_recomp.exe')],
                               cwd=root, env=env, stdout=stdout, stderr=stderr)
    (logs / 'game.pid').write_text(str(process.pid))
    print(f'PID {process.pid}; logs: analysis/{args.label}.*.log', flush=True)
    print(f'Exit code: {process.wait()}', flush=True)
