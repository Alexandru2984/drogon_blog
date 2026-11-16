#!/usr/bin/env python3
"""Publish a validated Vite build, retaining old chunks and replacing HTML last."""
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile

repo = Path(__file__).resolve().parent.parent
if len(sys.argv) not in (2, 3):
    raise SystemExit('usage: promote-frontend.py BUILD_DIR [LIVE_DIR]')
source = Path(sys.argv[1]).resolve()
target = Path(sys.argv[2]).resolve() if len(sys.argv) == 3 else repo / 'build/live-public'
if source == target or source in target.parents or target in source.parents:
    raise SystemExit('Build and live directories must be separate')
index = source / 'index.html'
if not index.is_file():
    raise SystemExit('Build has no index.html')
refs = re.findall(r'(?:src|href)=[\'"](/assets/[^\'"?#]+)', index.read_text())
if not refs:
    raise SystemExit('Build has no Vite asset references')
for ref in refs:
    asset = (source / ref.lstrip('/')).resolve()
    if source not in asset.parents or not asset.is_file():
        raise SystemExit('Build references a missing or invalid asset')
# Built assets are local files; never follow arbitrary symlinks into runtime
# data. The existing checkout's uploads link is deliberately excluded.
for path in source.rglob('*'):
    if path.is_symlink() and path.relative_to(source).parts[0] != 'uploads':
        raise SystemExit('Unexpected symlink in build')
target.mkdir(parents=True, exist_ok=True)
for entry in source.iterdir():
    if entry.name in ('index.html', 'uploads'):
        continue
    dest = target / entry.name
    if entry.is_dir():
        shutil.copytree(entry, dest, dirs_exist_ok=True)
    else:
        shutil.copy2(entry, dest)
uploads = target / 'uploads'
if not uploads.exists() and not uploads.is_symlink():
    uploads.symlink_to(repo / 'uploads', target_is_directory=True)
# Keep the previous entry point outside the document root for recovery.
if (target / 'index.html').is_file():
    previous = repo / 'build' / f'frontend-{target.name}.prev.html'
    previous.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(target / 'index.html', previous)
with tempfile.NamedTemporaryFile(dir=target, prefix='.index-', delete=False) as out:
    temporary = Path(out.name)
    out.write(index.read_bytes())
    out.flush()
    os.fsync(out.fileno())
try:
    temporary.chmod(0o644)
    os.replace(temporary, target / 'index.html')
finally:
    temporary.unlink(missing_ok=True)
print('Published frontend; previous hashed assets retained')
