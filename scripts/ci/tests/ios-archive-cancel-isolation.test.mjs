import assert from 'node:assert/strict';
import {execFileSync, spawn, spawnSync} from 'node:child_process';
import {copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, realpathSync, rmSync, writeFileSync} from 'node:fs';
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

const compiler = String.raw`#!/bin/bash
exec "$ARCHIVE_TEST_PYTHON" -c '
import json, os, signal, time
from pathlib import Path

root = Path(os.environ["IOS_CI_ROOT"])
probe = root / "next-checkout"
deadline = time.monotonic() + 15
assert os.environ["RUNNER_TRACKING_ID"] == "archive-fixture"

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
    if os.fork() == 0:
        os.setpgid(0, 0)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        os.write(2, b"separate-group grandchild holds compiler output\n")
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
    for (const path of [archiveScript, 'scripts/ci/ios-archive-supervisor.py', 'apps/browser/scripts/common.sh']) {
      mkdirSync(join(source, path, '..'), {recursive: true});
      copyFileSync(path, join(source, path));
    }
    mkdirSync(join(source, 'apps/ios/Aegis.xcodeproj'), {recursive: true});
    writeFileSync(join(source, 'apps/ios/Aegis.xcodeproj/project.pbxproj'), '// fixture\n');
    writeFileSync(join(bin, 'python3'), `#!/bin/bash
case "$3" in */ios-archive-supervisor.py) exec "$ARCHIVE_TEST_PYTHON" "$@" ;; esac
exit 0
`, {mode: 0o755});
    writeFileSync(join(bin, 'xcodebuild'), compiler, {mode: 0o755});

    // A sibling job follows the same log while the compiler is running.
    const log = join(root, 'xcodebuild.log');
    const loggerReady = join(root, 'logger-ready');
    writeFileSync(log, '');
    const otherJob = spawn(python, ['-c', `
import os, pathlib, sys, time
with open(sys.argv[1], "rb") as log:
    pathlib.Path(sys.argv[2]).write_text(str(os.getpid()))
    for _ in range(200):
        log.read()
        time.sleep(0.1)
`, log, loggerReady], {
      stdio: 'ignore',
    });
    const child = spawn('bash', [join(source, archiveScript)], {
      cwd: source,
      env: {...process.env, PATH: `${bin}:${process.env.PATH}`, IOS_CI_ROOT: root, IOS_CI_SOURCE: source,
        RUNNER_TRACKING_ID: 'archive-fixture', ARCHIVE_TEST_PYTHON: python, ARCHIVE_TEST_FAIL: signal ? '0' : '1'},
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let stdout = '';
    let diagnostic = '';
    child.stdout.on('data', data => { stdout += data; });
    child.stderr.on('data', data => { diagnostic += data; });
    let nextCheckoutStarted = false;
    child.once('exit', () => {
      writeFileSync(join(root, 'next-checkout'), 'next job checked out\n');
      nextCheckoutStarted = true;
    });
    const roles = ['compiler', 'helper', 'grandchild'];
    const ready = () => [loggerReady, ...roles.map(role => join(root, role))].every(path => existsSync(path));
    const marker = 'separate-group grandchild holds compiler output';

    try {
      await waitFor(() => nextCheckoutStarted || (ready() && stdout.includes(marker)), 'process markers and live diagnostic');
      assert(ready(), diagnostic);
      const [[compilerPid, supervisorPid, group, session], [helperPid, compilerParent, helperGroup, helperSession],
        [grandchildPid, helperParent, grandchildGroup, grandchildSession]] = roles.map(role =>
        JSON.parse(readFileSync(join(root, role), 'utf8')));
      assert.equal(supervisorPid, child.pid, 'the compiler must be the entry supervisor’s direct child');
      assert.deepEqual([group, session], [compilerPid, compilerPid]);
      assert.deepEqual([compilerParent, helperParent], [compilerPid, helperPid]);
      assert.deepEqual([helperGroup, grandchildGroup], [group, grandchildPid]);
      assert.deepEqual([helperSession, grandchildSession], [session, session]);

      if (signal) {
        assert(stdout.includes(marker), diagnostic);
        assert.equal(child.kill(signal), true);
      }
      await waitFor(() => nextCheckoutStarted, 'wrapper exit and next-checkout probe');
      assert.equal(child.exitCode, exitCode, diagnostic);
      assert(readFileSync(log, 'utf8').includes(marker));
      for (const pid of [compilerPid, helperPid, grandchildPid]) {
        assert.equal(running(pid), false, `archive descendant ${pid} survived wrapper exit`);
      }
      assert.equal(running(otherJob.pid), true, 'the unrelated log reader must survive');
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
  assert.match(workflow, /exec bash "\$IOS_CI_SOURCE\/scripts\/ci\/run-ios-unsigned-archive\.sh"/u);
  assert.match(workflow, /steps\.storage\.outputs\.source/u);
  assert.doesNotMatch(workflow, /xcodebuild .*\| tee/u);
  const probe = spawnSync('bash', ['-n', archiveScript], {encoding: 'utf8'});
  assert.equal(probe.status, 0, probe.stderr);
});
