"""Check the actual staged Git snapshot, including forced-added ignored files.

This catches common accidental disclosures, not copyright ownership or every secret.
"""
import argparse
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BLOCKED_EXT = {'.iso','.xex','.xexp','.exe','.dll','.pdb','.obj','.lib','.zip','.7z','.rar',
               '.rdc','.pix','.dmp','.caff','.ucode','.cso','.dxbc','.xsh','.dds','.xbw','.xma'}
BLOCKED_PARTS = {'.local','.deps','.git','out','reference','game','shader_dump','shader-dump',
                 'native_shaders','native_shader_cache','shadercache','__pycache__'}
ALLOWED_ROOTS = {'src','tools','config','cmake','sdk-patches','third_party','LICENSES','docs','.github'}
ALLOWED_TOP = {'README.md','LICENSE','.gitignore','.gitattributes','CMakeLists.txt','CMakePresets.json','THIRD_PARTY_NOTICES.md','CONTRIBUTING.md'}

def issues(path, data):
    p = PurePosixPath(path)
    found = []
    if p.is_absolute() or '..' in p.parts or '\\' in path:
        found.append('unsafe path')
    if any(part.lower() in BLOCKED_PARTS for part in p.parts) or p.suffix.lower() in BLOCKED_EXT:
        found.append('local/game/build artifact')
    if path.startswith('generated/') and path != 'generated/rexglue.cmake':
        found.append('game-derived generated output')
    if len(p.parts) == 1 and path not in ALLOWED_TOP:
        found.append('unreviewed top-level file')
    if len(p.parts) > 1 and p.parts[0] not in ALLOWED_ROOTS | {'generated'}:
        found.append('unreviewed directory')
    if path in {f'config/{n}.toml' for n in ('functions','engine_funcs','gpu_funcs','crt')}:
        found.append('unreviewed imported table')
    if len(data) > 4 * 1024 * 1024 or b'\x00' in data:
        found.append('binary or unexpectedly large file')
    for pattern in (rb'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----',
                    rb'gh[pousr]_[A-Za-z0-9]{30,}', rb'github_pat_[A-Za-z0-9_]{40,}'):
        if re.search(pattern, data): found.append('credential-like content')
    if p.name == 'nb_command_processor.cpp' and any(marker in data for marker in
            (b'kDofFogPsHlsl',b'kPostQuadVsHlsl',b'kSpriteVsHlsl',b'kSpritePsHlsl',b'kBloomPs',b'kBloomBlurHlsl')):
        found.append('embedded game shader transcription')
    return found

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--head', action='store_true', help='Check HEAD instead of the index (for CI).')
    args = parser.parse_args()
    listing = ['ls-tree','-r','-z','--full-tree','HEAD'] if args.head else ['ls-files','-s','-z']
    raw = subprocess.check_output(['git','-C',str(ROOT),*listing])
    errors, count = [], 0
    for entry in raw.split(b'\0'):
        if not entry: continue
        meta, encoded_path = entry.split(b'\t',1)
        mode, second, third = meta.decode().split()
        path = encoded_path.decode('utf-8')
        oid = third if args.head else second
        if not args.head and third != '0': errors.append(f'{path}: unmerged index entry'); continue
        if mode not in {'100644','100755'}: errors.append(f'{path}: symlink/submodule is not allowed'); continue
        data = subprocess.check_output(['git','-C',str(ROOT),'cat-file','blob',oid])
        errors.extend(f'{path}: {problem}' for problem in issues(path,data))
        count += 1
    if errors:
        print('\n'.join(errors),file=sys.stderr)
        return 1
    print(f'Publication file checks passed for {count} files. Ownership and provenance still require review.')
    return 0

if __name__ == '__main__': sys.exit(main())
