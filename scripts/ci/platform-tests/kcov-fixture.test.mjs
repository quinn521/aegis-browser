import assert from 'node:assert/strict';
import {execFileSync, spawnSync} from 'node:child_process';
import {lstatSync} from 'node:fs';
import {join} from 'node:path';
import test from 'node:test';

test('shell coverage runner parses and its Bash parent and child enforce the fixture contract', () => {
  execFileSync('bash', ['-n', join(import.meta.dirname, '../run-shell-coverage.sh')]);
  const parent = join(import.meta.dirname, 'kcov-parent-fixture.sh');
  const child = join(import.meta.dirname, 'kcov-child-fixture.sh');
  const entry = lstatSync(parent);
  assert.ok(entry.isFile());
  assert.equal(entry.mode & 0o111, 0);
  assert.equal(execFileSync('bash', [parent], {encoding: 'utf8'}), 'parent-and-child-covered\n');
  assert.equal(spawnSync('bash', [child, 'wrong-value']).status, 1);
});
