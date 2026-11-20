import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import {chmodSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync, existsSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import test from 'node:test';
import {setTimeout as delay} from 'node:timers/promises';

const archiveScript = 'scripts/ci/run-ios-unsigned-archive.sh';

function alive(pid) {
  try {
    process.kill(pid, 0);
    return true;
  } catch {
    return false;
  }
}

async function waitFor(predicate, label, attempts = 80, intervalMs = 100) {
  for (let index = 0; index < attempts; index += 1) {
    if (await predicate()) return;
    await delay(intervalMs);
  }
  throw new Error(`timed out waiting for ${label}`);
}

test('cancel reaps a TERM-ignoring archive child before the next checkout probe', async () => {
  const root = mkdtempSync(join(tmpdir(), 'ios-archive-cancel-'));
  const bin = join(root, 'bin');
  const marker = join(root, 'child.pid');
  const probe = join(root, 'next-checkout');
  const seen = join(root, 'seen');
  mkdirSync(bin);
  mkdirSync(join(root, 'Source/apps/ios/Aegis.xcodeproj'), {recursive: true});
  mkdirSync(join(root, 'Source/apps/ios/EmbeddedCore'), {recursive: true});
  writeFileSync(join(root, 'Source/apps/ios/Aegis.xcodeproj/project.pbxproj'), '// fixture\n');
  writeFileSync(join(root, 'Source/apps/ios/EmbeddedCore/embedded-core-lock.json'), '{}\n');
  for (const name of ['tmp', 'DerivedData', 'SourcePackages', 'PackageCache', 'ModuleCache']) {
    mkdirSync(join(root, name));
  }
  writeFileSync(seen, '');
  writeFileSync(join(bin, 'python3'), '#!/bin/bash\nexit 0\n');
  writeFileSync(join(bin, 'xcodebuild'), `#!/bin/bash
trap '' TERM
printf '%s\\n' "$$" > "$ARCHIVE_CHILD_MARKER"
while true; do
  if [[ -f "$ARCHIVE_NEXT_PROBE" ]]; then
    printf 'saw-next\\n' >> "$ARCHIVE_CHILD_SEEN"
    exit 0
  fi
  sleep 0.1
done
`);
  chmodSync(join(bin, 'python3'), 0o755);
  chmodSync(join(bin, 'xcodebuild'), 0o755);

  const child = spawn('bash', [archiveScript], {
    env: {
      ...process.env,
      PATH: `${bin}:${process.env.PATH}`,
      IOS_CI_ROOT: root,
      IOS_CI_SOURCE: join(root, 'Source'),
      ARCHIVE_CHILD_MARKER: marker,
      ARCHIVE_NEXT_PROBE: probe,
      ARCHIVE_CHILD_SEEN: seen,
    },
    stdio: ['ignore', 'pipe', 'pipe'],
  });

  try {
    await waitFor(() => existsSync(marker), 'archive child marker');
    const archiveChild = Number(readFileSync(marker, 'utf8').trim());
    assert.ok(Number.isInteger(archiveChild) && archiveChild > 0);
    assert.equal(alive(archiveChild), true);

    child.kill('SIGTERM');
    await waitFor(() => child.exitCode !== null || child.signalCode !== null, 'archive entry exit');
    assert.equal(child.exitCode, 143);

    await waitFor(() => !alive(archiveChild), 'archive child reap');
    writeFileSync(probe, 'next job checked out\n');
    await delay(400);
    assert.equal(readFileSync(seen, 'utf8'), '');
  } finally {
    if (alive(child.pid)) child.kill('SIGKILL');
    if (existsSync(marker)) {
      const leftover = Number(readFileSync(marker, 'utf8').trim());
      if (Number.isInteger(leftover) && alive(leftover)) process.kill(leftover, 'SIGKILL');
    }
    rmSync(root, {recursive: true, force: true});
  }
});

test('workflow keeps unsigned archive compilation behind the cancel-safe wrapper', () => {
  const workflow = readFileSync('.github/workflows/ios-release-validation.yml', 'utf8');
  assert.match(workflow, /run-ios-unsigned-archive\.sh/u);
  assert.match(workflow, /steps\.storage\.outputs\.source/u);
  assert.doesNotMatch(workflow, /xcodebuild .*\| tee/u);
  const probe = spawnSync('bash', ['-n', archiveScript], {encoding: 'utf8'});
  assert.equal(probe.status, 0, probe.stderr);
});
