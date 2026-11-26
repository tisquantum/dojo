"""Checks whether a change alters the simulation: builds the decompilation at a base commit and as it stands,
runs both through the same scripted VS bout, and compares their states variable by variable (statediff.py).

    simcheck.py [--base REF] [--fields F,F,...] [--keep-going]

REF (default HEAD) is a commit of the decompilation (DOJO_DECOMP_DIR, else the folder above port/). Both
builds use this port/, so the check is of the decompilation's changes and the port's uncommitted ones against
the same port. Builds are RelWithDebInfo (statediff needs their PDBs) in build/simcheck/, kept for the next
run; the base checkout is a git worktree there. The runs are hidden and use their own memory cards
(build/simcheck/run/saves). Prints statediff's report for each field and exits 1 if the simulation differs.

When it differs and the change is meant to alter the simulation, raise SIM_REVISION in src/online.c. """

import argparse
import os
import re
import shutil
import subprocess
import sys

PORT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DECOMP = os.path.abspath(os.environ.get('DOJO_DECOMP_DIR') or os.path.join(PORT, '..'))
WORK = os.path.join(PORT, 'build', 'simcheck')
TOOLCHAIN = os.path.join(PORT, 'toolchain')
LLVM = r'C:\Program Files\LLVM\bin'
DISC_NAME = 'Bushido Blade (USA).bin'

# Variables that differ between any two builds without the simulation differing (see README, statediff).
KNOWN_NOISE = {
    r'gArenaPeakBytes': 'a write-only statistic',
    r'gBillboard(\d+Uvs|Textures)':'unused kinds; from the stack in builds before SIM_REVISION 6, zero since',
}

# Title, main menu, VS, both players' picks: a bout from about field 1870 (see README, DOJO_PRESS).
MENU_PRESSES = ['500:start', '620:down', '660:x'] + ['%d:x+p2x' % f for f in range(900, 1900, 100)]
FIGHT_BUTTONS = ['s', 't', 'x', 'o', 'up', 'down', 'left', 'right', 'l1', 'r1', 'x+right', 's+up', 't+down']
DEFAULT_FIELDS = '1000,1700,2200,2800,3400,4200,5000'


def fight_presses(first, last):
    """Both players pressing buttons chosen by a fixed generator, so every run fights the same bout."""
    seed = 12345
    out = []
    for field in range(first, last, 9):
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        p1 = FIGHT_BUTTONS[(seed >> 8) % len(FIGHT_BUTTONS)]
        p2 = FIGHT_BUTTONS[(seed >> 16) % len(FIGHT_BUTTONS)]
        out.append('%d:%s+%s' % (field, p1, '+'.join('p2' + b for b in p2.split('+'))))
    return out


def run(args, **kw):
    print('>', ' '.join(args) if len(' '.join(args)) < 160 else args[0] + ' ...', flush=True)
    return subprocess.run(args, check=True, **kw)


def env():
    e = dict(os.environ)
    e['PATH'] = LLVM + os.pathsep + e.get('PATH', '')
    return e


def junction(link, target):
    """A directory junction (no administrator rights needed). Removed with os.rmdir only, never rmtree, which
    on older Pythons follows junctions."""
    if not os.path.exists(link):
        subprocess.run(['cmd', '/c', 'mklink', '/J', link, target], check=True, stdout=subprocess.DEVNULL)


def checkout_base(ref):
    src = os.path.join(WORK, 'base-src')
    commit = subprocess.run(['git', '-C', DECOMP, 'rev-parse', '--verify', ref + '^{commit}'], check=True,
                            capture_output=True, text=True).stdout.strip()
    if not os.path.exists(os.path.join(src, '.git')):
        run(['git', '-C', DECOMP, 'worktree', 'prune'])
        run(['git', '-C', DECOMP, 'worktree', 'add', '--detach', src, commit])
    else:
        run(['git', '-C', src, 'checkout', '--quiet', '--force', '--detach', commit])
    return src, commit


def build(name, decomp):
    out = os.path.join(WORK, name)
    disc = os.path.join(DECOMP, 'game_files', DISC_NAME)
    if not os.path.exists(os.path.join(out, 'build.ninja')):
        run([os.path.join(TOOLCHAIN, 'cmake', 'bin', 'cmake.exe'), '-S', PORT, '-B', out, '-G', 'Ninja',
             '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
             '-DCMAKE_C_COMPILER=' + os.path.join(TOOLCHAIN, 'llvm-mingw', 'bin', 'i686-w64-mingw32-clang.exe'),
             '-DCMAKE_MAKE_PROGRAM=' + os.path.join(TOOLCHAIN, 'ninja', 'ninja.exe'),
             '-DDOJO_DECOMP_DIR=' + decomp.replace('\\', '/'), '-DDOJO_DISC=' + disc.replace('\\', '/')],
            env=env(), stdout=subprocess.DEVNULL)
    log = open(os.path.join(WORK, name + '.log'), 'w')
    try:
        run([os.path.join(TOOLCHAIN, 'ninja', 'ninja.exe'), '-C', out], env=env(), stdout=log, stderr=log)
    except subprocess.CalledProcessError:
        sys.exit('simcheck: the %s build failed; see %s' % (name, log.name))
    return os.path.join(out, 'dojo.exe'), os.path.join(out, 'dojo.pdb')


def play(name, exe, fields):
    """Runs the scripted bout; returns field -> (state file, its stderr log)."""
    rundir = os.path.join(WORK, 'run')
    os.makedirs(rundir, exist_ok=True)
    junction(os.path.join(rundir, 'game_files'), os.path.join(DECOMP, 'game_files'))
    saves = os.path.join(rundir, 'saves')
    if os.path.isdir(saves) and not os.path.islink(saves):
        shutil.rmtree(saves)
    last = max(fields)
    e = env()
    e.update(DOJO_HIDDEN='1', DOJO_FAST='1', DOJO_EXIT_FIELD=str(last + 1),
             DOJO_DUMP_STATE=','.join(map(str, fields)),
             DOJO_PRESS=','.join(MENU_PRESSES + fight_presses(1900, last)))
    log_path = os.path.join(WORK, 'run-%s.txt' % name)
    with open(log_path, 'w') as log:
        result = subprocess.run([exe], cwd=rundir, env=e, stdout=subprocess.DEVNULL, stderr=log, timeout=900)
    states = re.findall(r'field (\d+): game state (\d+)', open(log_path, errors='replace').read())
    print('%s: exit %d; game states %s' % (name, result.returncode,
                                           ' '.join('%s@%s' % (s, f) for f, s in states[3:])))
    out = {}
    for field in fields:
        src = os.path.join(rundir, 'state_%06d.bin' % field)
        if not os.path.exists(src):
            sys.exit('simcheck: the %s run wrote no state at field %d; see %s' % (name, field, log_path))
        dst = os.path.join(WORK, '%s_%06d.bin' % (name, field))
        os.replace(src, dst)
        out[field] = dst
    return out, log_path


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--base', default='HEAD')
    ap.add_argument('--fields', default=DEFAULT_FIELDS)
    ap.add_argument('--keep-going', action='store_true', help='compare every field, not only to the first that differs')
    a = ap.parse_args()
    fields = sorted(int(f) for f in a.fields.split(','))
    os.makedirs(WORK, exist_ok=True)

    base_src, commit = checkout_base(a.base)
    print('base %s (%s), against the decompilation in %s' % (a.base, commit[:10], DECOMP))
    base_exe, base_pdb = build('base', base_src)
    head_exe, head_pdb = build('head', DECOMP)
    base_states, base_log = play('base', base_exe, fields)
    head_states, head_log = play('head', head_exe, fields)

    statediff = os.path.join(PORT, 'tools', 'statediff.py')
    differs = False
    for field in fields:
        report = subprocess.run([sys.executable, statediff, base_states[field], base_log, base_pdb,
                                 head_states[field], head_log, head_pdb],
                                capture_output=True, text=True, env=env()).stdout
        real = []
        for line in report.splitlines():
            m = re.match(r'(\S+)\s+size (\d+)/(\d+), (\d+) bytes differ', line)
            if m:
                name, bytes_differ = m.group(1).rstrip("'"), int(m.group(4))
                noise = [why for pattern, why in KNOWN_NOISE.items() if re.fullmatch(pattern, name)]
                if noise:
                    line += '   (known: %s)' % noise[0]
                elif bytes_differ == 0:
                    line += '   (size only: the layout changed, not the values)'
                else:
                    real.append(name)
            elif re.match(r'(psx ram|i/o page): [1-9]', line):
                real.append(line.split(':')[0])
            if not line.endswith(' variables differ'):
                print('  ' + line)
        print('field %d: %s' % (field, 'SIMULATION DIFFERS: ' + ' '.join(real) if real else 'same'))
        if real:
            differs = True
            if not a.keep_going:
                break
    print('simcheck: %s' % ('the simulation differs from %s' % a.base if differs else 'the simulation is unchanged'))
    sys.exit(1 if differs else 0)


main()
