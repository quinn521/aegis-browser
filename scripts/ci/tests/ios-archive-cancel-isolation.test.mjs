import assert from 'node:assert/strict';
import {execFileSync, spawn, spawnSync} from 'node:child_process';
import {existsSync, mkdirSync, mkdtempSync, readFileSync, realpathSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import test from 'node:test';
import {setTimeout as delay} from 'node:timers/promises';

const archiveScript = 'scripts/ci/run-ios-unsigned-archive.sh';

function running(pid) {
  try {
    process.kill(pid, 0);
  } catch (error) {
    if (error.code === 'ESRCH') return false;
    if (error.code !== 'EPERM') throw error;
  }
  // A reparented zombie cannot read the next checkout, even if its PID
  // survives briefly after the wrapper exits.
  const state = spawnSync('/bin/ps', ['-p', String(pid), '-o', 'stat='], {encoding: 'utf8'});
  if (state.error) throw state.error;
  if (state.status === 1 && !state.stdout.trim()) return false;
  assert.equal(state.status, 0, state.stderr);
  return !state.stdout.trim().startsWith('Z');
}

async function waitFor(predicate, label) {
  for (let index = 0; index < 160; index += 1) {
    if (predicate()) return;
    await delay(50);
  }
  throw new Error(`timed out waiting for ${label}`);
}

const compiler = `#!/bin/bash
exec "$ARCHIVE_TEST_PYTHON" -c '
import json, os, signal, time
from pathlib import Path

root = Path(os.environ["IOS_CI_ROOT"])
probe = root / "next-checkout"
deadline = time.monotonic() + 15

def mark(role):
    status = [os.getpid(), os.getppid(), os.getpgrp(), os.getsid(0)]
    ready = root / (role + ".ready")
    ready.write_text(json.dumps(status))
    ready.rename(root / role)

def watch():
    while root.is_dir() and time.monotonic() < deadline:
        if probe.exists():
            (root / "saw-next-checkout").touch()
            return
        time.sleep(0.01)

if os.fork() == 0:
    with open(os.devnull, "r+") as quiet:
        for descriptor in (0, 1, 2):
            os.dup2(quiet.fileno(), descriptor)
    if os.fork() == 0:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        mark("grandchild")
    else:
        mark("helper")
    watch()
    os._exit(0)

mark("compiler")
if os.environ["ARCHIVE_TEST_FAIL"] == "1":
    while not (root / "helper").exists() or not (root / "grandchild").exists():
        if time.monotonic() >= deadline:
            raise SystemExit(1)
        time.sleep(0.01)
    raise SystemExit(65)
watch()
'
`;

for (const [signal, exitCode] of [['SIGTERM', 143], ['SIGINT', 130], [null, 65]]) {
  test(`archive ${signal ?? 'compiler failure'} stops owned descendants before wrapper exit`, async () => {
    const root = mkdtempSync(join(realpathSync(tmpdir()), 'ios-archive-cancel-|'));
    const bin = join(root, 'bin');
    const source = join(root, 'Source');
    const python = execFileSync('python3', ['-c', 'import sys; print(sys.executable)'], {encoding: 'utf8'}).trim();
    mkdirSync(bin);
    mkdirSync(join(source, 'apps/ios/Aegis.xcodeproj'), {recursive: true});
    writeFileSync(join(source, 'apps/ios/Aegis.xcodeproj/project.pbxproj'), '// fixture\n');
    writeFileSync(join(bin, 'python3'), `#!/bin/bash
case "$3" in */ios-archive-supervisor.py) exec "$ARCHIVE_TEST_PYTHON" "$@" ;; esac
exit 0
`, {mode: 0o755});
    writeFileSync(join(bin, 'xcodebuild'), compiler, {mode: 0o755});

    // A sibling job can refer to the exact archive log path in its argv.
    const otherJob = spawn(python, ['-c', 'import time; time.sleep(20)', join(root, 'xcodebuild.log')], {
      stdio: 'ignore',
    });
    const child = spawn('bash', [archiveScript], {
      env: {...process.env, PATH: `${bin}:${process.env.PATH}`, IOS_CI_ROOT: root, IOS_CI_SOURCE: source,
        ARCHIVE_TEST_PYTHON: python, ARCHIVE_TEST_FAIL: signal ? '0' : '1'},
      stdio: ['ignore', 'ignore', 'pipe'],
    });
    let diagnostic = '';
    child.stderr.on('data', data => { diagnostic += data; });
    let nextCheckoutStarted = false;
    child.once('exit', () => {
      writeFileSync(join(root, 'next-checkout'), 'next job checked out\n');
      nextCheckoutStarted = true;
    });
    const roles = ['compiler', 'helper', 'grandchild'];

    try {
      await waitFor(() => roles.every(role => existsSync(join(root, role))), 'owned process markers');
      const [[compilerPid, workerPid, group, session], [helperPid, compilerParent, helperGroup, helperSession],
        [grandchildPid, helperParent, grandchildGroup, grandchildSession]] = roles.map(role =>
        JSON.parse(readFileSync(join(root, role), 'utf8')));
      assert.equal(group, workerPid, 'the archive shell must lead a private process group');
      assert.deepEqual([compilerParent, helperParent], [compilerPid, helperPid]);
      assert.deepEqual([helperGroup, grandchildGroup], [group, group]);
      assert.deepEqual([helperSession, grandchildSession], [session, session]);
      assert.notEqual(session, group, 'the worker must stay in the Actions session');

      if (signal) assert.equal(child.kill(signal), true);
      await waitFor(() => nextCheckoutStarted, 'wrapper exit and next-checkout probe');
      assert.equal(child.exitCode, exitCode, diagnostic);
      for (const pid of [workerPid, compilerPid, helperPid, grandchildPid]) {
        assert.equal(running(pid), false, `archive descendant ${pid} survived wrapper exit`);
      }
      assert.equal(process.kill(otherJob.pid, 0), true, 'the unrelated log reader must survive');
      assert.equal(existsSync(join(root, 'saw-next-checkout')), false);
    } finally {
      child.removeAllListeners('exit');
      if (child.exitCode === null && child.signalCode === null) child.kill('SIGTERM');
      otherJob.kill('SIGTERM');
      rmSync(root, {recursive: true, force: true});
    }
  });
}

test('workflow keeps unsigned archive compilation behind the cancel-safe wrapper', () => {
  const workflow = readFileSync('.github/workflows/ios-release-validation.yml', 'utf8');
  assert.match(workflow, /exec bash scripts\/ci\/run-ios-unsigned-archive\.sh/u);
  assert.match(workflow, /steps\.storage\.outputs\.source/u);
  assert.doesNotMatch(workflow, /xcodebuild .*\| tee/u);
  const probe = spawnSync('bash', ['-n', archiveScript], {encoding: 'utf8'});
  assert.equal(probe.status, 0, probe.stderr);
});
