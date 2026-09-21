#!/usr/bin/env python3
"""Build the pinned API-permission runtime, then offer supervised feature upgrade."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
REPO = Path(__file__).resolve().parents[1]
FEATURE = '06b9c82264bbcfdf6cebf9583f8a7985f32a8683'


def require(value):
    if not value:
        raise RuntimeError('preflight_failed')


def git(*args):
    result = subprocess.run(['git', '-C', str(REPO), *args], capture_output=True, timeout=30)
    require(result.returncode == 0)
    return result.stdout.decode().strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-only', action='store_true')
    args = parser.parse_args()
    stage = 'preflight'
    try:
        require(os.geteuid() != 0 and sys.stdin.isatty())
        os.umask(0o077)
        require(REPO.resolve() == REPO and REPO.stat().st_uid == os.getuid())
        require(git('rev-parse', '--show-toplevel') == str(REPO))
        require(git('branch', '--show-current') == 'alarm-users-v1')
        require(not git('status', '--porcelain', '--untracked-files=normal'))
        require(git('rev-parse', '--verify', FEATURE + '^{commit}') == FEATURE)
        git('merge-base', '--is-ancestor', FEATURE, 'HEAD')
        for directory in ('.local-builds', '.local-backups'):
            git('check-ignore', '--quiet', '--no-index', directory + '/privacy-probe')
            require(not git('ls-files', '--', directory))
            path = REPO / directory
            require(not path.is_symlink())
            path.mkdir(mode=0o700, exist_ok=True)
            require(path.stat().st_uid == os.getuid())
            path.chmod(0o700)
        # Use a disposable local fixture test; do not access household services.
        stage = 'maintenance_tests'
        logs = Path(tempfile.mkdtemp(prefix='api-upgrade-checks.', dir=REPO / '.local-builds'))
        with (logs / 'maintenance-tests.log').open('w') as output:
            tests = subprocess.run([sys.executable, '-B', '-m', 'unittest', 'discover',
                                    '-s', 'tests', '-p', 'test_garage_deconz_maintenance.py'],
                                   cwd=REPO, stdout=output, stderr=subprocess.STDOUT)
        require(tests.returncode == 0)
        print('Offline maintenance/recovery tests passed.', flush=True)
        stage = 'build'
        existing = set((REPO / '.local-builds').iterdir())
        print('Building baseline and the pinned feature; services stay running. This can take several minutes.', flush=True)
        print('Build logs stay private in .local-builds. Do not paste them into chat.', flush=True)
        with (logs / 'build.log').open('w') as output:
            build = subprocess.run(['bash', 'tools/build-alarm-users.sh', FEATURE], cwd=REPO,
                                   stdout=output, stderr=subprocess.STDOUT)
        require(build.returncode == 0)
        candidates = []
        for path in set((REPO / '.local-builds').iterdir()) - existing:
            if not path.is_symlink() and path.is_dir() and (path / 'versions.txt').is_file():
                versions = dict(line.split('=', 1) for line in (path / 'versions.txt').read_text().splitlines() if '=' in line)
                if versions.get('feature') == FEATURE:
                    candidates.append(path)
        require(len(candidates) == 1)
        folder = candidates[0]
        require('PASS: 223 checks' in (folder / 'tests.log').read_text())
        print('Both builds and all 223 access-policy checks passed.', flush=True)
        print('Private staged build: ' + str(folder), flush=True)
        if args.build_only:
            print(json.dumps({'completed': True, 'stage': 'build_only', 'services_changed': False}))
            return 0
        stage = 'supervised_install'
        print('Next: supervised upgrade. Start physically CLOSED, LOCKED and DISARMED; keep controls unused.', flush=True)
        print('The installer takes a fresh private backup and asks UPGRADE, then STILL. It sends no movement commands.', flush=True)
        result = subprocess.run(['sudo', '/usr/bin/python3', '-B',
            str(REPO / 'tools/garage-deconz-maintenance.py'), 'upgrade-feature', '--build-dir', str(folder)])
        if result.returncode != 0:
            print('Installation did not complete. Follow its sanitized summary; do not retry movement or clear faults.')
            return result.returncode
        print('Installation completed. Existing-code compatibility testing is next; no user or PIN changes were requested.')
        return 0
    except (Exception, KeyboardInterrupt):
        print(json.dumps({'completed': False, 'stage': stage, 'private_details_omitted': True}))
        if stage == 'supervised_install':
            print('Keep controls unused; review the installer receipt before recovery. Do not clear faults.')
        else:
            print('Installation was not started. Existing services were not changed by this runner.')
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
