#!/usr/bin/env python3
"""Append cmake module staging lines (D/F) to ext2_manifest.txt from
build/ports/cmake-share/share layout."""

import os

REPO = os.path.dirname(os.path.abspath(__file__))
share = os.path.join(REPO, 'build', 'ports', 'cmake-share', 'share')
manifest = os.path.join(REPO, 'ext2_manifest.txt')

lines = ['', '# ---- CMake modules/templates (needed by tools/cmake-wynlandos', '#    binary to locate its CMAKE_ROOT at /usr/share/cmake-3.28) ----',
         'D /usr/share/cmake-3.28']

base_img = '/usr/share/cmake-3.28'
for root, dirs, files in os.walk(share):
    rel = os.path.relpath(root, share)  # e.g. Modules/Platform
    img_dir = base_img if rel == '.' else f'{base_img}/{rel}'.replace('\\', '/')
    if rel != '.':
        lines.append(f'D {img_dir}')
    for f in sorted(files):
        host = os.path.join(root, f)
        img_path = f'{img_dir}/{f}'
        lines.append(f'F {img_path} {os.path.relpath(host, REPO).replace(os.sep, "/")}')

with open(manifest, 'a', encoding='utf-8') as fp:
    fp.write('\n'.join(lines) + '\n')

n_dirs = sum(1 for l in lines if l.startswith('D '))
n_files = sum(1 for l in lines if l.startswith('F '))
print(f'appended: {n_dirs} dirs, {n_files} files')
