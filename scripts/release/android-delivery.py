#!/usr/bin/env python3
"""Android Actions transport; real package/sign/store work stays in ./release."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('android_plan', Path(__file__).with_name('android-delivery-plan.py'))
codec = importlib.util.module_from_spec(spec)
spec.loader.exec_module(codec)
require = codec.require
RECORD = re.compile(r'(?:play-upload|android-(?:distribute|submit))-(?:started|result)\.json\Z')


def run(*argv, capture=False):
    result = subprocess.run(list(map(str, argv)), cwd=ROOT, text=True, capture_output=capture)
    require(result.returncode == 0, 'Delivery command failed; retain evidence and reconcile before retry')
    return result.stdout.strip() if capture else None


def api(path):
    repository = os.environ['GITHUB_REPOSITORY']
    require(re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository) and '..' not in repository, 'Invalid repository')
    return json.loads(run('gh', 'api', '-H', 'Accept: application/vnd.github+json', 'repos/'+repository+'/'+path, capture=True))


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as content:
        for chunk in iter(lambda: content.read(1024*1024), b''):
            h.update(chunk)
    return h.hexdigest()


def source():
    return dict(sha=run('git', 'rev-parse', 'HEAD', capture=True), tree=run('git', 'rev-parse', 'HEAD^{tree}', capture=True), dirty=False)


def plan():
    value = json.loads(os.environ['ANDROID_DELIVERY_PLAN'])
    # The planner is the only producer; re-normalize before trusting a JSON plan in a later job.
    fields = {'operation', 'profile', 'version', 'build', 'format', 'destination', 'track', 'tag', 'play_release_policy'}
    pins = {'package_run', 'package_attempt', 'state_run', 'state_attempt', 'manifest_sha256'}
    require(isinstance(value, dict) and not value.keys()-(fields | pins) and fields <= value.keys(), 'Invalid delivery plan')
    op = value['operation']
    full = {k: value[k] for k in ['operation', 'destination', 'track', 'tag', 'play_release_policy'] if value[k]}
    target = f'{value["version"]}({value["build"]})' if op == 'package' else codec.encode(value)
    fresh = codec.normalize(dict(operation='full', product=value['profile'], target=target, advanced=json.dumps(full)), codec.native_identity(ROOT))
    require(value == fresh, 'Delivery plan identity changed')
    return value


def directory(p):
    producer = p.get('package_run', os.environ['GITHUB_RUN_ID'])
    attempt = p.get('package_attempt', os.environ['GITHUB_RUN_ATTEMPT'])
    require(re.fullmatch(codec.NUMBER, producer) and re.fullmatch(codec.NUMBER, attempt), 'Invalid package producer')
    return ROOT/'.artifacts/release/android'/f'android-gh-{producer}-{attempt}'


def arguments(p, stage):
    args = [ROOT/'release', 'android', stage, '--profile', p['profile'], '--version', p['version'], '--build', p['build'],
            '--format', 'aab', '--destination', p['destination'], '--run-id', directory(p).name]
    for key in ['tag', 'track', 'play_release_policy', 'manifest_sha256']:
        if p.get(key):
            args += ['--'+key.replace('_', '-'), p[key]]
    if p['destination'] == 'github-draft':
        args += ['--repository', os.environ['GITHUB_REPOSITORY']]
    return args


def verify_package(folder, p, expected, exact_source):
    manifest_path = folder/'android-manifest.json'
    require(digest(manifest_path) == expected and (folder/'android-manifest.json.sha256').read_text() == f'{expected}  android-manifest.json\n', 'Original manifest digest mismatch')
    m = json.loads(manifest_path.read_text())
    require(m.get('schemaVersion') == 1 and m.get('repository') == 'gcsagroup/aegis-browser' and m.get('source') == exact_source and
            m.get('releaseReady') is False and m.get('applicationId') == 'com.gcsa.aegis' and m.get('platform') == 'android' and
            m.get('format') == 'aab' and all(m.get(k) == p[k] for k in ['version', 'build', 'profile']), 'Original package identity mismatch')
    require(m.get('details', {}).get('signing') == 'android-upload-key' and
            re.fullmatch(codec.DIGEST, m['details'].get('certificateSha256', '')), 'Package must carry verified Upload-key AAB evidence')
    assets = m.get('assets')
    require(isinstance(assets, list) and 0 < len(assets) <= 20, 'Invalid manifest assets')
    names = [a['name'] for a in assets]
    require(len(set(names)) == len(names) and sum(n.endswith('.aab') for n in names) == 1, 'Expected one real AAB')
    allowed = {'android-manifest.json', 'android-manifest.json.sha256', 'delivery-origin.json'}
    for a in assets:
        name = a['name']
        require(re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*', name) and '..' not in name and name.endswith(('.aab', '.build-identity.json')), 'Invalid Android artifact name')
        path = folder/name
        require(not path.is_symlink() and path.is_file() and path.stat().st_size == a['size'] and digest(path) == a['sha256'], 'Android artifact bytes changed')
        allowed.add(name)
    for path in folder.iterdir():
        if RECORD.fullmatch(path.name):
            require(path.is_file() and not path.is_symlink(), 'Invalid stage record')
            record = json.loads(path.read_text())
            intent = record.get('intent', record)
            require(intent.get('manifestSha256') == expected and all(intent.get(k) == m.get(k) for k in ['platform', 'applicationId', 'version', 'build', 'profile', 'format']), 'Stage record identity changed')
            allowed.add(path.name)
    return allowed


def verify_run(row, run_id, attempt, sha, repository):
    require(row.get('id') == int(run_id) and row.get('run_attempt') == int(attempt) and row.get('head_sha') == sha and
            row.get('repository', {}).get('full_name') == repository and row.get('head_repository', {}).get('full_name') == repository and
            row.get('path') == '.github/workflows/android-delivery.yml' and row.get('event') == 'workflow_dispatch' and
            row.get('status') == 'completed' and row.get('conclusion') == 'success', 'Producer/source/current attempt is not a successful Android delivery')


def extract_flat(archive, folder):
    require(not folder.exists() and not any(path.is_symlink() for path in [folder, *folder.parents]), 'Import destination must be new with no symlink parents')
    with zipfile.ZipFile(archive) as z:
        rows = z.infolist()
        names = [r.filename for r in rows]
        require(0 < len(rows) <= 40 and len(names) == len(set(names)) and sum(r.file_size for r in rows) <= 4*1024**3, 'Invalid artifact ZIP size/members')
        for row in rows:
            name = row.filename
            require(re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*', name) and '..' not in name and not row.is_dir() and
                    not stat.S_ISLNK(row.external_attr >> 16) and
                    (name.endswith(('.aab', '.build-identity.json')) or name in {'android-manifest.json', 'android-manifest.json.sha256', 'delivery-origin.json'} or RECORD.fullmatch(name)), 'Unexpected artifact ZIP member')
        folder.mkdir(parents=True, mode=0o700)
        for row in rows:
            with z.open(row) as src, (folder/row.filename).open('xb') as dst:
                shutil.copyfileobj(src, dst)


def verify_producers(p, s):
    repository = os.environ['GITHUB_REPOSITORY']
    for key, att in [('package_run', 'package_attempt'), ('state_run', 'state_attempt')]:
        verify_run(api('actions/runs/'+p[key]), p[key], p[att], s['sha'], repository)
        jobs = api(f'actions/runs/{p[key]}/attempts/{p[att]}/jobs?per_page=100')
        require(isinstance(jobs.get('total_count'), int) and jobs['total_count'] <= 100, 'Incomplete producer jobs inventory')
        expected = 'package' if key == 'package_run' or p['state_run'] == p['package_run'] else 'store'
        rows = [job for job in jobs.get('jobs', []) if job.get('name') == expected]
        require(len(rows) == 1 and rows[0].get('status') == 'completed' and rows[0].get('conclusion') == 'success', 'Required producer job has not succeeded')


def import_state(p):
    repository = os.environ['GITHUB_REPOSITORY']
    s = source()
    verify_producers(p, s)
    rows = api('actions/runs/'+p['state_run']+'/artifacts?per_page=100')
    require(isinstance(rows.get('total_count'), int) and rows['total_count'] <= 100, 'Incomplete artifact inventory')
    name = f'android-delivery-{p["state_run"]}-{p["state_attempt"]}'
    selected = [a for a in rows.get('artifacts', []) if a['name'] == name and not a['expired']]
    require(len(selected) == 1 and isinstance(selected[0].get('id'), int), 'Exact producer artifact missing or ambiguous')
    with tempfile.TemporaryDirectory(prefix='android-delivery-') as tmp:
        outer = Path(tmp)/'artifact.zip'
        with outer.open('xb') as out:
            subprocess.run(['gh', 'api', '-H', 'Accept: application/vnd.github+json', f'repos/{repository}/actions/artifacts/{selected[0]["id"]}/zip'], cwd=ROOT, stdout=out, stderr=subprocess.DEVNULL, check=True)
        with zipfile.ZipFile(outer) as z:
            rows = z.infolist()
            require(len(rows) == 1 and rows[0].filename == 'package.zip' and rows[0].file_size <= 4*1024**3 and not stat.S_ISLNK(rows[0].external_attr >> 16), 'Invalid transport envelope')
            archive = Path(tmp)/'package.zip'
            with z.open(rows[0]) as src, archive.open('xb') as dst:
                shutil.copyfileobj(src, dst)
        extract_flat(archive, directory(p))
    allowed = verify_package(directory(p), p, p['manifest_sha256'], s)
    require({f.name for f in directory(p).iterdir()} == allowed, 'Unexpected imported state')
    origin = json.loads((directory(p)/'delivery-origin.json').read_text())
    require(origin == dict(repository=repository, source=s, **{k: p[k] for k in codec.RECEIPT_FIELDS}), 'Delivery origin differs from receipt')
    # Check current attempts again after potentially slow downloads.
    verify_producers(p, s)


def export_state(p, success):
    folder = directory(p)
    require(folder.exists(), 'No package state to export')
    expected = p.get('manifest_sha256') or digest(folder/'android-manifest.json')
    allowed = verify_package(folder, p, expected, source())
    pins = {k: p[k] for k in ['profile', 'version', 'build']}
    pins.update(package_run=p.get('package_run', os.environ['GITHUB_RUN_ID']), package_attempt=p.get('package_attempt', os.environ['GITHUB_RUN_ATTEMPT']),
                state_run=os.environ['GITHUB_RUN_ID'], state_attempt=os.environ['GITHUB_RUN_ATTEMPT'], manifest_sha256=expected)
    # Failed attempts preserve journals but are never admitted as successful source runs.
    (folder/'delivery-origin.json').write_text(json.dumps(dict(repository=os.environ['GITHUB_REPOSITORY'], source=source(), **pins), indent=2)+'\n')
    target = ROOT/'.artifacts/android-delivery/package.zip'
    target.parent.mkdir(parents=True, exist_ok=True)
    require(not target.exists(), 'Export attempt already exists')
    with zipfile.ZipFile(target, 'x', compression=zipfile.ZIP_STORED) as z:
        for name in sorted(allowed):
            z.write(folder/name, name)
    if success:
        value = codec.encode(pins)
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as summary:
            summary.write('### Android 交付凭据 / Delivery receipt\n\n等待整次运行成功，再复制到下次“版本或来源”。\nCopy after the entire run succeeds.\n\n```text\n'+value+'\n```\n\nPlay Upload creates a draft only. Tester availability and device acceptance require separate evidence.\n')


def main():
    p = plan()
    command = sys.argv[1]
    require(source()['sha'] == os.environ['GITHUB_SHA'], 'Checkout differs from runtime source')
    run('node', ROOT/'scripts/ci/android-operator.mjs')
    if command == 'import':
        import_state(p)
    elif command == 'package':
        require(p['operation'] == 'package', 'Package route required')
        run(*arguments(p, 'package'))
        export_state(p, True)
    elif command == 'stage':
        require(p['operation'] in {'upload', 'status', 'distribute', 'submit'}, 'Explicit existing-state stage required')
        require(os.environ['GITHUB_RUN_ATTEMPT'] == '1' or p['operation'] == 'status', 'Mutations cannot be replayed by rerunning Actions; reconcile retained intent first')
        verify_package(directory(p), p, p['manifest_sha256'], source())
        verify_producers(p, source())
        if p['destination'] == 'github-draft':
            run(*arguments(p, 'upload'))
            return
        try:
            run(*arguments(p, p['operation']))
        except BaseException:
            export_state(p, False)
            raise
        export_state(p, True)
    else:
        raise ValueError('Unknown Android adapter command')


if __name__ == '__main__':
    try:
        main()
    except Exception:
        print('Android delivery failed; inspect retained evidence and remote state before retrying', file=sys.stderr)
        sys.exit(1)
