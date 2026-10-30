# Copyright VR Robotica. All Rights Reserved.

"""
Fab upload zips of the plugin's last commit, one per engine version.

    python Scripts/fab_zip.py                         -> a zip for every version in VERSIONS, on the desktop
    python Scripts/fab_zip.py 5.6 5.7                 -> MaterialRerouteTools_5.6.zip and _5.7.zip on the desktop
    python Scripts/fab_zip.py 5.6 out.zip             -> one version to a chosen path

Zips the committed uplugin, README, Config, Resources and Source (not the working copy, and never Binaries,
Intermediate or this folder) into one MaterialRerouteTools/ folder with forward-slash paths, sets the uplugin's
EngineVersion to <engine version>.0 and checks the result. Commit before zipping: uncommitted edits are left out.
"""
import os
import subprocess
import sys
import zipfile

PLUGIN = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))	# the folder above Scripts/
SHIPPED = ['MaterialRerouteTools.uplugin', 'README.md', 'Config', 'Resources', 'Source']
VERSIONS = ['5.6', '5.7', '5.8']	# engine versions the plugin is built and tested on


def git(*args):
    return subprocess.run(['git', '-C', PLUGIN, *args], capture_output=True, check=True).stdout


def main():
    args = sys.argv[1:]
    if any(a in ('-h', '--help') for a in args):
        sys.exit(__doc__)
    if len(args) == 2 and args[1].endswith('.zip'):
        jobs = [(args[0], args[1])]
    else:
        jobs = [(v, os.path.join(desktop(), 'MaterialRerouteTools_%s.zip' % v)) for v in (args or VERSIONS)]
    for version, _ in jobs:
        if version.count('.') != 1 or not version.replace('.', '').isdigit():
            sys.exit('error: "%s" is not an engine version like 5.7' % version)

    if git('status', '--porcelain', '--', *SHIPPED).strip():
        print('warning: uncommitted changes in the shipped files are not in the zips')
    for version, out in jobs:
        make_zip(version, out)


def make_zip(version, out):
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for f in git('ls-files', '--', *SHIPPED).decode().split():
            data = git('show', 'HEAD:' + f)
            if f == 'MaterialRerouteTools.uplugin':
                data = set_engine_version(data, version)
            z.writestr('MaterialRerouteTools/' + f, data)

    check(out, version)
    print(out)


def set_engine_version(uplugin, version):
    lines = uplugin.decode().splitlines(keepends=True)
    found = False
    for i, line in enumerate(lines):
        if '"EngineVersion"' in line:
            indent = line[:len(line) - len(line.lstrip())]
            ending = '\r\n' if line.endswith('\r\n') else '\n'
            lines[i] = '%s"EngineVersion": "%s.0",%s' % (indent, version, ending)
            found = True
    if not found:
        sys.exit('error: no "EngineVersion" in MaterialRerouteTools.uplugin')
    return ''.join(lines).encode()


def check(out, version):
    z = zipfile.ZipFile(out)
    names = [n for n in z.namelist() if not n.endswith('/')]
    uplugin = z.read('MaterialRerouteTools/MaterialRerouteTools.uplugin').decode()
    problems = []
    if '"EngineVersion": "%s.0"' % version not in uplugin:
        problems.append('EngineVersion is not %s.0' % version)
    problems += ['bad path: ' + n for n in names if '\\' in n or '/Binaries/' in n or '/Intermediate/' in n]
    problems += ['no copyright line: ' + n for n in names
                 if n.endswith(('.cpp', '.h', '.cs')) and not z.read(n).startswith(b'// Copyright')]
    print('%d files from %s' % (len(names), git('log', '-1', '--format=%h %s').decode().strip()))
    if problems:
        sys.exit('\n'.join(['error:'] + problems))


def desktop():
    # OneDrive moves the desktop on many machines
    for path in (os.path.join(os.environ.get('OneDrive', ''), 'Desktop'), os.path.join(os.path.expanduser('~'), 'Desktop')):
        if os.path.isdir(path):
            return path
    return os.getcwd()


if __name__ == '__main__':
    main()
