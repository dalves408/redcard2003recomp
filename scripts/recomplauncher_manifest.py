"""RecompLauncher manifest (recomplauncher.json, SDK Level 3) and release ZIP for RedCard 2003.

    python scripts/recomplauncher_manifest.py --version 1.0.0 [--build build/Release] [--out installer/Output]
    python scripts/recomplauncher_manifest.py --json-only            # print the manifest

Writes <build>/recomplauncher.json (the installer ships it too) and
<out>/RedCard2003-v<version>-Windows-x64.zip (+ .sha256) holding the game, config.exe,
redcard_import.exe, the icon and recomplauncher.json. See https://recomplauncher.com/sdk.

Level 1: start "Redcard 2003.exe"; the player's USA disc image or folder (default.xbe check).
Level 2: the settings in %LOCALAPPDATA%\\RedCard2003\\settings.ini (the keys F1 and config.exe use).
Level 3: prepare = redcard_import --prepare (copies and verifies game_files, "progress" lines);
         game.dumps = Redump's USA disc.
"""
import argparse
import hashlib
import json
import sys
import zipfile
from pathlib import Path

# Redump, Microsoft Xbox, "RedCard 20-03" USA, serial MW-015 (http://redump.org/disc/54255/).
# The Europe "RedCard" (disc 9429) is a different build; only the USA default.xbe is supported.
DUMPS = [{'name': 'USA', 'size': 7825162240, 'crc32': '630af9ec'}]

SAVE_ROOT = '{local}/RedCard 2003'        # main.c: UDATA, TDATA, TitleCache; logs/ (release log)
SETTINGS = '{local}/RedCard2003/settings.ini'

PROGRAM = 'Redcard 2003.exe'
FILES = [  # (built file, name in the ZIP)
    ('redcard_recomp.exe', PROGRAM),
    ('config.exe', 'config.exe'),
    ('redcard_import.exe', 'redcard_import.exe'),
]


def _row(identifier, label, key, choices, default, help_text, group=None):
    row = {
        'id': identifier,
        'label': label,
        'help': help_text,
        'choices': [{'label': name, 'set': {key: str(value)}} for name, value in choices],
        'defaults': {key: str(default)},
    }
    if group:
        row['group'] = group
    return row


def manifest():
    toggle = [('Off', 0), ('On', 1)]
    settings = [
        _row('scale', 'Render scale', 'display/resolution_scale',
             [('1x (640x480, original)', 1), ('2x (1280x960)', 2), ('3x (1920x1440)', 3), ('4x (2560x1920)', 4)], 1,
             'How sharp the game is drawn. 1x is the Xbox\'s own; higher needs a faster graphics card.', 'Video'),
        _row('fullscreen', 'Fullscreen', 'display/fullscreen', toggle, 0,
             'Borderless fullscreen. Alt+Enter switches while playing. Big Picture mode always starts fullscreen.',
             'Video'),
        _row('stretch', 'Stretch image', 'display/stretch', [('Off (4:3)', 0), ('On (fill the window)', 1)], 0,
             'Off keeps the original 4:3 picture with bars at the sides.', 'Video'),
        _row('vsync', 'VSync', 'display/vsync', toggle, 1,
             'Waits for your screen\'s refresh: no tearing.', 'Video'),
        _row('frame_rate', 'Frame rate', 'display/frame_rate',
             [('30', 30), ('60 (original)', 60), ('120', 120), ('144', 144), ('240', 240), ('Uncapped (up to 360)', 360)],
             60, '60 is the original. The game runs on its own clock, so other rates change how smooth it looks.',
             'Video'),
        _row('msaa', 'Anti-aliasing', 'display/msaa', [('Off', 1), ('2x MSAA', 2), ('4x MSAA', 4), ('8x MSAA', 8)], 1,
             'Smooths jagged edges. Costs more at higher render scales.', 'Video'),
        _row('anisotropy', 'Texture filtering', 'display/anisotropy',
             [('Off', 1), ('2x', 2), ('4x', 4), ('8x', 8), ('16x', 16)], 1,
             'Keeps the pitch and stands sharp at shallow angles.', 'Video'),
        _row('textures', 'Hi-res textures', 'textures/enabled', toggle, 1,
             'The 4x texture pack. Only has an effect when it is installed (the Windows Setup can download it).',
             'Video'),
    ]
    return {
        '$schema': 'https://recomplauncher.com/schema/recomplauncher.json',
        'format': 1,
        'name': 'RedCard 2003',
        'repo': 'dalves408/redcard2003recomp',
        'game': {
            'kind': 'iso_or_folder',
            'label': ('Your RedCard 20-03 Xbox disc image (USA, serial MW-015), or the folder with its files '
                      '(the one that holds default.xbe).'),
            'check': {'file': 'default.xbe', 'xdvdfs': 'default.xbe'},
            'dumps': DUMPS,
        },
        'start': {'program': [PROGRAM]},
        'prepare': {
            'program': ['redcard_import.exe'],
            'args': ['--prepare', '{game}', '{install}/game_files'],
            'label': 'Game files from your disc',
            'timeout': 900,
            'done': ['{install}/game_files/default.xbe'],
        },
        'config': [{
            'id': 'main',
            'format': 'ini',
            'file': [{'path': SETTINGS}],
            # A new install starts fullscreen (as the Windows Setup does); the game's own default
            # for a missing key stays windowed, so existing players keep their setting.
            'new': {**{key: value for row in settings for key, value in row['defaults'].items()},
                    'display/fullscreen': '1'},
        }],
        'settings': settings,
        'saves': [SAVE_ROOT + '/UDATA', SAVE_ROOT + '/TDATA'],
        'folders': {
            'saves': [{'path': SAVE_ROOT + '/UDATA'}],
            'logs': [
                {'path': '{install}/logs', 'if': '{install}/portable.txt'},
                {'path': SAVE_ROOT + '/logs'},
            ],
        },
        'actions': [
            {'id': 'saves', 'label': 'Saves', 'help': 'Your profiles, seasons and settings saved by the game.',
             'open': 'saves'},
            {'id': 'logs', 'label': 'Logs', 'help': 'A log of each of the last 10 times the game ran.',
             'open': 'logs'},
        ],
        'exit_codes': {
            '0': 'ok',
            '2': 'Your copy of the game could not be found. Pick it again on the game\'s page.',
            '3': 'That file is not an Xbox disc image.',
            '4': 'That is not the supported version of RedCard 20-03 (the Xbox USA release, MW-015).',
            '5': 'The disc image is damaged or incomplete.',
            '6': 'The game files could not be written to the game\'s folder. Check free space.',
        },
        'looks': {'graphics': 'd3d11', 'lines': 480},
    }


def main():
    here = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--version', default='1.0.0', help='release version, e.g. 1.0.0')
    parser.add_argument('--build', type=Path, default=here / 'build' / 'Release')
    parser.add_argument('--out', type=Path, default=here / 'installer' / 'Output')
    parser.add_argument('--suffix', default='', help='added to the ZIP name, e.g. -test1')
    parser.add_argument('--json-only', action='store_true', help='print recomplauncher.json and exit')
    args = parser.parse_args()
    text = json.dumps(manifest(), indent=2) + '\n'
    if args.json_only:
        sys.stdout.write(text)
        return
    (args.build / 'recomplauncher.json').write_text(text, encoding='utf-8', newline='\n')
    args.out.mkdir(parents=True, exist_ok=True)
    archive = args.out / f'RedCard2003-v{args.version}{args.suffix}-Windows-x64.zip'
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as package:
        for built, name in FILES:
            package.write(args.build / built, name)
        package.write(here / 'resources' / 'redcard_2003.ico', 'redcard_2003.ico')
        package.writestr('recomplauncher.json', text)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_name(archive.name + '.sha256').write_text(f'{digest}  {archive.name}\n')
    print(f'recomplauncher.json: {args.build / "recomplauncher.json"}')
    print(f'RecompLauncher ZIP: {archive}\nSHA-256: {digest}')


if __name__ == '__main__':
    main()
