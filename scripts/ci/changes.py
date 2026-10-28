#!/usr/bin/env python3
"""Select builds from the push/PR base; unknown bases require all checks."""
import os
import subprocess

base = os.environ.get('CHANGE_BASE', '')
full = os.environ.get('FULL_CHECKS') == 'true'
if not base or set(base) == {'0'}:
    full = True
elif subprocess.run(['git', 'cat-file', '-e', base + '^{commit}'],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
    full = True
paths = [] if full else subprocess.check_output(
    ['git', 'diff', '--name-only', '-z', base, 'HEAD']).decode().split('\0')
common = ('src/core/', 'src/app/', 'src/install/', 'src/platform/', 'src/probe/',
          'vendor/', 'tests/', 'scripts/ci/', '.github/workflows/', '.gitea/workflows/')
pc = full or any(p.startswith(common) or p in
                 ('Makefile', 'Makefile.pc', '.gitmodules', 'src/main_pc.c') for p in paths)
switch = full or any(p.startswith(('src/', 'vendor/', 'resources/')) or p in
                     ('CMakeLists.txt', 'Makefile', 'Makefile.switch', '.gitmodules') or
                     p.startswith(('scripts/', '.github/workflows/', '.gitea/workflows/'))
                     for p in paths)
outputs = {'pc': str(pc).lower(), 'switch': str(switch).lower(),
           'scan_range': '' if full else base + '..HEAD'}
for name, value in outputs.items():
    print(f'{name}={value}')
with open(os.environ['GITHUB_OUTPUT'], 'a') as output:
    for name, value in outputs.items():
        output.write(f'{name}={value}\n')
