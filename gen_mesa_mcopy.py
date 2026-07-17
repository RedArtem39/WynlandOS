import subprocess, os

deps = set()
out = subprocess.check_output(['ldd', '/usr/lib/x86_64-linux-gnu/libEGL_mesa.so.0', '/usr/lib/x86_64-linux-gnu/dri/swrast_dri.so', '/usr/lib/x86_64-linux-gnu/libGLESv2.so.2']).decode()
for x in out.split('\n'):
    if '=>' in x and len(x.split()) >= 3 and x.split()[2].startswith('/'):
        deps.add(x.split()[2])

deps.add('/usr/lib/x86_64-linux-gnu/libEGL_mesa.so.0')
deps.add('/usr/lib/x86_64-linux-gnu/libGLESv2.so.2')

mcopy_cmds = [f'\t@mcopy -i $@ {lib} ::/lib64/{os.path.basename(lib)}' for lib in deps]
mcopy_cmds.append('\t-@mmd -i $@ ::/lib64/dri')
mcopy_cmds.append('\t@mcopy -i $@ /usr/lib/x86_64-linux-gnu/dri/swrast_dri.so ::/lib64/dri/swrast_dri.so')
mcopy_cmds.append('\t-@mmd -i $@ ::/etc/glvnd')
mcopy_cmds.append('\t-@mmd -i $@ ::/etc/glvnd/egl_vendor.d')
mcopy_cmds.append('\t@mcopy -i $@ /usr/share/glvnd/egl_vendor.d/50_mesa.json ::/etc/glvnd/egl_vendor.d/50_mesa.json')

with open('mesa_mcopy.txt', 'w') as f:
    f.write('\n'.join(mcopy_cmds) + '\n')
