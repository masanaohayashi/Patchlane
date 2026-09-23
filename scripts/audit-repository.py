#!/usr/bin/env python3
"""Audit the prospective Git file set without initializing the project repository."""
import argparse
import subprocess
import tempfile
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--forbid', action='append', default=[], help='Case-insensitive forbidden text; may be repeated')
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='patchlane-audit-') as directory:
    subprocess.run(['git', 'init', '--quiet', '--bare', directory], check=True)
    result = subprocess.run(['git', '--git-dir', directory, '--work-tree', str(root),
                             'ls-files', '--others', '--exclude-standard', '-z'],
                            cwd=root, capture_output=True, check=True)
paths = sorted(p.decode() for p in result.stdout.split(b'\0') if p)
failures = []
for relative in paths:
    path = root / relative
    if path.is_symlink():
        failures.append(f'{relative}: symlink requires review')
        continue
    raw = path.read_bytes()
    # Only the reviewed README screenshot is an allowed binary asset.
    if relative == 'docs/images/patchlane.png':
        if not raw.startswith(b'\x89PNG\r\n\x1a\n'):
            failures.append(f'{relative}: invalid PNG signature')
        continue
    try:
        text = raw.decode('utf-8')
    except UnicodeDecodeError:
        failures.append(f'{relative}: unexpected binary file')
        continue
    for forbidden in args.forbid:
        if forbidden.casefold() in (relative + '\n' + text).casefold():
            failures.append(f'{relative}: forbidden text')
    if ('/' + 'Users/') in text or ('/' + 'var/folders/') in text:
        failures.append(f'{relative}: machine-specific absolute path')
    if ('-----BEGIN ' + 'PRIVATE KEY-----') in text or ('-----BEGIN ' + 'RSA PRIVATE KEY-----') in text:
        failures.append(f'{relative}: private key material')
output = root / '.build/repository-files.txt'
output.parent.mkdir(exist_ok=True)
output.write_text('\n'.join(paths) + '\n')
if failures:
    raise SystemExit('\n'.join(failures))
print(f'PASS: {len(paths)} publication files; name/content checks passed')
print('Manifest: .build/repository-files.txt')
