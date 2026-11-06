"""Content-keyed reuse of audited ARM64 core libraries (no fixed APK/commit pin)."""
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

LIBRARIES = {'libmain.so': 'libmain.so', 'libSDL3.so': '3rdparty/SDL3/libSDL3.so'}
LIBRARIES.update({n: '3rdparty/libadrenotools/src/hook/' + n for n in (
    'libhook_impl.so', 'libmain_hook.so', 'libfile_redirect_hook.so', 'libgsl_alloc_hook.so')})
NATIVE_EXTENSIONS = {'.c', '.cc', '.cpp', '.cxx', '.h', '.hpp', '.s', '.S', '.asm', '.cmake', '.in'}
BUILD_STEPS = ('Android NDK', 'Configure', 'Build native libraries', 'Stage native libraries')

def native_input(path):
    if path == 'android/ci/native_cache.py':
        return True
    if path.startswith('android/'):
        return Path(path).suffix in NATIVE_EXTENSIONS
    return not (path.startswith(('docs/', 'tests/', '.github/')) or
                path.endswith('.md') or path in {'.gitignore', '.gitattributes'})

def compatibility_key(entries, workflow, image_version=''):
    digest = hashlib.sha256(b'kyty-arm64-core-v1\0')
    for path, mode, oid in sorted(entries):
        if native_input(path):
            digest.update(f'{mode} {oid} {path}\n'.encode())
    ndk = re.search(r'^  NDK_VERSION: (\S+)$', workflow, re.M)
    if not ndk:
        raise ValueError('NDK version missing')
    digest.update(ndk.group(1).encode())
    for step in BUILD_STEPS:
        found = re.search(r'      - name: ' + re.escape(step) +
                          r'\n(.*?)(?=\n      - |\Z)', workflow, re.S)
        if not found:
            raise ValueError('Native build step missing: ' + step)
        digest.update(found.group(0).encode())
    digest.update(image_version.encode())
    return digest.hexdigest()

def git(*args):
    return subprocess.check_output(['git', *args]).decode()

def current_key():
    raw = subprocess.check_output(['git', 'ls-tree', '-rz', 'HEAD'])
    entries = []
    for line in raw.split(b'\0'):
        if not line:
            continue
        metadata, path = line.split(b'\t', 1)
        mode, _, oid = metadata.decode().split()
        entries.append((path.decode(), mode, oid))
    for path in git('diff', 'HEAD', '--name-only').splitlines():
        if native_input(path) or path == '.github/workflows/android.yml':
            raise ValueError('Uncommitted native build input: ' + path)
    return compatibility_key(entries, Path('.github/workflows/android.yml').read_text(),
                             os.environ.get('ImageVersion', ''))

def library_hashes(stage):
    hashes = {}
    for name, relative in LIBRARIES.items():
        data = (stage / relative).read_bytes()
        if len(data) < 64 or data[:5] != b'\x7fELF\x02' or data[5] != 1 or struct.unpack_from('<H', data, 18)[0] != 183:
            raise ValueError('Expected an ARM64 ELF library: ' + name)
        hashes[name] = hashlib.sha256(data).hexdigest()
    return hashes

def verify(stage, key):
    manifest = json.loads((stage / 'native-cache.json').read_text())
    if manifest['compatibility_key'] != key or manifest['libraries'] != library_hashes(stage):
        raise ValueError('Cached native core signature or library hashes differ')
    return manifest

def record_asset(manifest):
    dest = Path('android/app/src/main/assets/native-core.json')
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(json.dumps(manifest, indent=2) + '\n')

def output(name, value):
    with open(os.environ['GITHUB_OUTPUT'], 'a') as stream:
        stream.write(f'{name}={value}\n')

def main():
    command = sys.argv[1]
    key = current_key()
    if command == 'key':
        output('key', key)
        print('Native compatibility key:', key)
        return
    stage = Path(os.environ['RUNNER_TEMP']) / 'native'
    if command == 'verify':
        try:
            manifest = verify(stage, key)
            record_asset(manifest)
            reused = True
            print('Reusing byte-verified native core from', manifest['source_commit'])
        except (OSError, ValueError, KeyError) as error:
            reused = False
            print('Native core cache miss or mismatch; compiling with incremental object/FEX caches:', error)
        output('reused', str(reused).lower())
    elif command == 'seal':
        manifest = {'compatibility_key': key, 'source_commit': git('rev-parse', 'HEAD').strip(),
                    'ndk_version': os.environ['NDK_VERSION'], 'libraries': library_hashes(stage)}
        (stage / 'native-cache.json').write_text(json.dumps(manifest, indent=2) + '\n')
        record_asset(manifest)
    else:
        raise ValueError('Unknown command: ' + command)

if __name__ == '__main__':
    main()
