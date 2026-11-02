#!/usr/bin/env python3
"""Install outside checkout, read-only, with one host-verified run/attempt pin."""
import json
import os
from pathlib import Path
import re
import signal
import sys
import time

OWNER = '01a19ada-b833-7bf0-9309-d860785b13fe'


def require(ok):
    if not ok:
        raise ValueError('Android runner admission rejected')


def validate(e, config):
    fields = {'repository','repositoryId','sourceSha','runId','attempt','actorId','actor','job','workflow','ref','ownerThreadId'}
    require(set(config) == fields and all(isinstance(v,str) and v for v in config.values()))
    require(config['repository'] == 'quinn521/aegis-browser' and config['repositoryId'] == '1363949860' and
            config['ownerThreadId'] == OWNER and re.fullmatch('[a-f0-9]{40}',config['sourceSha']) and
            all(re.fullmatch('[1-9][0-9]{0,15}',config[k]) for k in ['runId','attempt','actorId']))
    require(config['workflow'] in {'android-release-validation.yml','android-delivery.yml','android-delivery-full.yml','release-candidate-tag.yml'})
    native = config['workflow'] == 'android-release-validation.yml' and config['job'] == 'native-admission'
    package = config['workflow'] in {'android-delivery.yml','android-delivery-full.yml','release-candidate-tag.yml'} and config['job'] == 'package'
    require(native or package)
    tag = config['workflow'] == 'release-candidate-tag.yml'
    require(re.fullmatch(r'refs/tags/candidate/android/[A-Za-z0-9][A-Za-z0-9_.-]{0,95}',config['ref']) and '..' not in config['ref'] if tag
            else config['ref'] == 'refs/heads/codex/android-release-acceptance')
    for name,key in [('GITHUB_REPOSITORY','repository'),('GITHUB_REPOSITORY_ID','repositoryId'),('GITHUB_SHA','sourceSha'),
                     ('GITHUB_RUN_ID','runId'),('GITHUB_RUN_ATTEMPT','attempt'),('GITHUB_ACTOR_ID','actorId'),
                     ('GITHUB_ACTOR','actor'),('GITHUB_JOB','job'),('GITHUB_REF','ref')]:
        require(e.get(name) == config[key])
    require(e.get('GITHUB_TRIGGERING_ACTOR') == config['actor'])
    require(e.get('GITHUB_EVENT_NAME') == ('push' if tag or native else 'workflow_dispatch'))
    require(e.get('GITHUB_WORKFLOW_REF') == config['repository']+'/.github/workflows/'+config['workflow']+'@'+config['ref'])
    event_path = Path(e['GITHUB_EVENT_PATH'])
    require(event_path.stat().st_size <= 8*1024*1024)
    payload = json.loads(event_path.read_text())
    require(payload.get('repository',{}).get('id') == int(config['repositoryId']) and
            payload['repository'].get('full_name') == config['repository'])


def abort_own_worker(proc=Path('/proc')):
    pid = os.getppid()
    expected = Path('/opt/runner/bin/Runner.Worker')
    for _ in range(12):
        require(pid > 1)
        entry = proc/str(pid)
        require(entry.stat().st_uid == os.geteuid())
        executable = (entry/'exe').resolve(strict=True)
        if executable == expected:
            os.kill(pid, signal.SIGKILL)
            return
        text = (entry/'stat').read_text()
        pid = int(text[text.rfind(')')+2:].split()[1])
    raise ValueError('Could not verify owning worker')


def main():
    try:
        path = Path('/opt/aegis-admission/pin.json')
        require(not path.is_symlink() and path.stat().st_size < 8192)
        validate(os.environ,json.loads(path.read_text()))
    except Exception:
        print('Android dedicated runner rejected this job before checkout',file=sys.stderr,flush=True)
        try:
            abort_own_worker()
        except Exception:
            # A failed hook alone can leave always() steps eligible. Block
            # synchronously until the task owner stops this isolated container.
            while True:
                time.sleep(30)
        return 1
    print('Android dedicated runner admitted the exact pinned job',flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
