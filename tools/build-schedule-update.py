#!/usr/bin/env python3
"""Build committed schedule source and its tests only; never stop/install services."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
def git(*args):
    result = subprocess.run(['git','-C',str(ROOT),*args],capture_output=True,timeout=30)
    if result.returncode:raise RuntimeError()
    return result.stdout.decode().strip()
def main():
    stage='checkout'
    try:
        if os.geteuid()==0 or git('branch','--show-current')!='alarm-users-v1' or git('status','--porcelain','--untracked-files=normal'):
            raise RuntimeError()
        revision=git('rev-parse','HEAD')
        for name in ('.local-builds','.local-backups'):
            git('check-ignore','--quiet','--no-index',name+'/probe')
            if git('ls-files','--',name):raise RuntimeError()
            path=ROOT/name
            if path.is_symlink():raise RuntimeError()
            path.mkdir(mode=0o700,exist_ok=True)
            if path.stat().st_uid!=os.getuid():raise RuntimeError()
            path.chmod(0o700)
        os.umask(0o077)
        logs=Path(tempfile.mkdtemp(prefix='schedule-checks.',dir=ROOT/'.local-builds'))
        stage='build'
        print('Building baseline and schedule feature; all existing services stay running.',flush=True)
        print('Full logs stay private. This can take several minutes.',flush=True)
        with (logs/'build.log').open('w') as out:
            result=subprocess.run(['bash','tools/build-alarm-users.sh',revision],cwd=ROOT,stdout=out,stderr=subprocess.STDOUT)
        if result.returncode:raise RuntimeError()
        print(json.dumps({'completed':True,'stage':'build_only','feature_commit':revision,
                          'baseline_and_feature_built':True,'store_and_qt_schedule_tests_passed':True,
                          'services_changed':False,'installed':False}))
        print('Installation needs a separately reviewed runtime pin and supervised maintenance step.')
        return 0
    except (Exception,KeyboardInterrupt):
        print(json.dumps({'completed':False,'stage':stage,'services_changed':False,'private_details_omitted':True}))
        print('Do not paste build logs. Nothing was installed or restarted.')
        return 1
if __name__=='__main__':raise SystemExit(main())
