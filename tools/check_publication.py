"""Check the actual staged Git snapshot, including forced-added ignored files.

This catches common accidental disclosures, not copyright ownership or every secret.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
BLOCKED_EXT = {'.iso','.xex','.xexp','.exe','.dll','.pdb','.obj','.lib','.zip','.7z','.rar',
               '.rdc','.pix','.dmp','.caff','.ucode','.cso','.dxbc','.xsh','.dds','.xbw','.xma'}
BLOCKED_PARTS = {'.local','.deps','.git','out','reference','game','shader_dump','shader-dump',
                 'native_shaders','native_shader_cache','shadercache','__pycache__'}
ALLOWED_ROOTS = {'src','tools','config','cmake','sdk-patches','third_party','LICENSES','docs','.github'}
ALLOWED_TOP = {'README.md','LICENSE','.gitignore','.gitattributes','CMakeLists.txt','CMakePresets.json','THIRD_PARTY_NOTICES.md','CONTRIBUTING.md'}
SCREENSHOT_MANIFEST = 'docs/screenshots/manifest.json'
SCREENSHOT_PREFIX = 'docs/screenshots/'
MAX_SCREENSHOT_BYTES = 24 * 1024 * 1024

def png_issues(data):
    """Validate the bounded, metadata-free PNG format used for reviewed screenshots."""
    if len(data) > MAX_SCREENSHOT_BYTES:
        return ['screenshot exceeds 24 MiB']
    if not data.startswith(b'\x89PNG\r\n\x1a\n'):
        return ['invalid PNG signature']
    offset, seen, image_bytes, palette_entries = 8, set(), 0, 0
    color_type = None
    allowed = {b'IHDR', b'PLTE', b'IDAT', b'IEND', b'tRNS', b'sRGB', b'gAMA', b'cHRM', b'pHYs'}
    fixed_lengths = {b'IHDR': 13, b'IEND': 0, b'sRGB': 1, b'gAMA': 4, b'cHRM': 32, b'pHYs': 9}
    while offset < len(data):
        if len(data) - offset < 12:
            return ['truncated PNG chunk']
        length, chunk = struct.unpack_from('>I4s', data, offset)
        end = offset + length + 12
        if end > len(data):
            return ['invalid PNG chunk length']
        payload = data[offset + 8:end - 4]
        crc = struct.unpack_from('>I', data, end - 4)[0]
        if zlib.crc32(chunk + payload) & 0xffffffff != crc:
            return ['invalid PNG chunk CRC']
        if chunk not in allowed:
            return ['PNG contains metadata or an unsupported chunk']
        if chunk != b'IDAT' and chunk in seen:
            return ['duplicate PNG chunk']
        if chunk in fixed_lengths and length != fixed_lengths[chunk]:
            return ['invalid PNG chunk size']
        if not seen and chunk != b'IHDR':
            return ['PNG must begin with IHDR']
        if b'IDAT' in seen and chunk not in {b'IDAT', b'IEND'}:
            return ['invalid PNG chunk order']
        if chunk == b'IHDR':
            width, height, depth, color_type, compression, filtering, interlace = struct.unpack('>IIBBBBB', payload)
            depths = {0: {1, 2, 4, 8, 16}, 2: {8, 16}, 3: {1, 2, 4, 8}, 4: {8, 16}, 6: {8, 16}}
            if not (0 < width <= 16384 and 0 < height <= 16384 and width * height <= 100_000_000):
                return ['invalid or excessive PNG dimensions']
            if depth not in depths.get(color_type, set()) or compression or filtering or interlace not in {0, 1}:
                return ['invalid PNG image format']
        elif chunk == b'PLTE':
            if not length or length % 3 or length > 768 or color_type in {0, 4}:
                return ['invalid PNG palette']
            palette_entries = length // 3
        elif chunk == b'tRNS':
            if not ((color_type == 0 and length == 2) or (color_type == 2 and length == 6)
                    or (color_type == 3 and 0 < length <= palette_entries)):
                return ['invalid PNG transparency']
        elif chunk == b'IDAT':
            if color_type == 3 and not palette_entries:
                return ['indexed PNG is missing a palette']
            image_bytes += length
        elif chunk == b'IEND':
            if not image_bytes:
                return ['PNG is missing image data']
            if end != len(data):
                return ['PNG has trailing payload']
            return []
        seen.add(chunk)
        offset = end
    return ['PNG is missing IEND']


def screenshot_manifest(snapshot):
    """Read only the manifest present in the same Git snapshot as the images."""
    if SCREENSHOT_MANIFEST not in snapshot:
        return {}, []
    try:
        manifest = json.loads(snapshot[SCREENSHOT_MANIFEST])
    except (ValueError, UnicodeDecodeError):
        return {}, ['screenshot manifest is not valid JSON']
    if not isinstance(manifest, dict) or not isinstance(manifest.get('images'), list):
        return {}, ['screenshot manifest must contain an images array']
    images, errors = {}, []
    for entry in manifest['images']:
        if not isinstance(entry, dict):
            errors.append('screenshot manifest entry must be an object')
            continue
        filename, digest = entry.get('file'), entry.get('sha256')
        if not isinstance(filename, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*\.png', filename):
            errors.append('screenshot manifest file must be an immediate PNG basename')
            continue
        path = SCREENSHOT_PREFIX + filename
        if path in images:
            errors.append(f'duplicate screenshot manifest entry: {filename}')
            continue
        if not isinstance(digest, str) or not re.fullmatch(r'[0-9a-f]{64}', digest):
            errors.append(f'invalid screenshot SHA-256: {filename}')
            continue
        images[path] = digest
        if path not in snapshot:
            errors.append(f'listed screenshot is absent from the Git snapshot: {filename}')
    return images, errors


def issues(path, data, screenshot_sha256=None):
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
    listed_png = (screenshot_sha256 is not None and path.startswith(SCREENSHOT_PREFIX)
                  and re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*\.png', path[len(SCREENSHOT_PREFIX):]))
    reviewed_png = False
    if listed_png:
        image_errors = png_issues(data)
        if hashlib.sha256(data).hexdigest() != screenshot_sha256:
            image_errors.append('screenshot does not match its reviewed SHA-256')
        found.extend(image_errors)
        reviewed_png = not image_errors
    elif p.suffix.lower() == '.png':
        found.append('PNG is not listed in the screenshot manifest')
    if not reviewed_png and (len(data) > 4 * 1024 * 1024 or b'\x00' in data):
        found.append('binary or unexpectedly large file')
    for pattern in (rb'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----',
                    rb'gh[pousr]_[A-Za-z0-9]{30,}', rb'github_pat_[A-Za-z0-9_]{40,}'):
        if re.search(pattern, data): found.append('credential-like content')
    if p.name == 'nb_command_processor.cpp' and any(marker in data for marker in
            (b'kDofFogPsHlsl',b'kPostQuadVsHlsl',b'kSpriteVsHlsl',b'kSpritePsHlsl',b'kBloomPs',b'kBloomBlurHlsl')):
        found.append('embedded game shader transcription')
    return found


def snapshot_issues(snapshot):
    images, manifest_errors = screenshot_manifest(snapshot)
    errors = [f'{SCREENSHOT_MANIFEST}: {problem}' for problem in manifest_errors]
    for path, data in snapshot.items():
        errors.extend(f'{path}: {problem}' for problem in issues(path, data, images.get(path)))
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--head', action='store_true', help='Check HEAD instead of the index (for CI).')
    args = parser.parse_args()
    listing = ['ls-tree','-r','-z','--full-tree','HEAD'] if args.head else ['ls-files','-s','-z']
    raw = subprocess.check_output(['git','-C',str(ROOT),*listing])
    errors, snapshot = [], {}
    for entry in raw.split(b'\0'):
        if not entry: continue
        meta, encoded_path = entry.split(b'\t',1)
        mode, second, third = meta.decode().split()
        path = encoded_path.decode('utf-8')
        oid = third if args.head else second
        if not args.head and third != '0': errors.append(f'{path}: unmerged index entry'); continue
        if mode not in {'100644','100755'}: errors.append(f'{path}: symlink/submodule is not allowed'); continue
        data = subprocess.check_output(['git','-C',str(ROOT),'cat-file','blob',oid])
        snapshot[path] = data
    errors.extend(snapshot_issues(snapshot))
    if errors:
        print('\n'.join(errors),file=sys.stderr)
        return 1
    print(f'Publication file checks passed for {len(snapshot)} files. Ownership and provenance still require review.')
    return 0

if __name__ == '__main__': sys.exit(main())
