import assert from 'node:assert/strict';
import {mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {spawnSync} from 'node:child_process';
import test from 'node:test';
import YAML from 'yaml';

import {
  classifyBranchRelationship,
  closedPullBlocks,
  inferPromotionTitle,
  parseConventionalTitle,
  qualityGateState,
} from '../promotion-orchestrator.mjs';

const root = resolve(import.meta.dirname, '../../..');

test('promotion title chooses the last feature and strips PR suffixes', () => {
  assert.equal(inferPromotionTitle([
    'feat(access): add request dispatch block barriers (#46)',
    'fix(access): address review feedback',
    'feat(access): enforce request dispatch gate (#51)',
    'fix(ci): stabilize fixture',
  ]), 'feat(access): enforce request dispatch gate');
});

test('promotion title falls back to the last non-release Conventional Commit', () => {
  assert.equal(inferPromotionTitle([
    'release: promote develop through PR #46',
    'docs(ci): document promotion automation',
    'fix(ci): fail closed on divergent main',
  ]), 'fix(ci): fail closed on divergent main');
  assert.equal(inferPromotionTitle(['release: promotion wrapper', 'Merge branch main']), null);
  assert.equal(parseConventionalTitle('ci: automate promotion (#52)')?.title, 'ci: automate promotion');
});

test('branch relationship classification is fail closed for divergence', () => {
  assert.equal(classifyBranchRelationship({originMain: 'a', upstreamMain: 'a', originMainAncestor: true, upstreamMainAncestor: true}), 'same');
  assert.equal(classifyBranchRelationship({originMain: 'a', upstreamMain: 'b', originMainAncestor: true, upstreamMainAncestor: false}), 'origin-behind');
  assert.equal(classifyBranchRelationship({originMain: 'b', upstreamMain: 'a', originMainAncestor: false, upstreamMainAncestor: true}), 'origin-ahead');
  assert.equal(classifyBranchRelationship({originMain: 'a', upstreamMain: 'b', originMainAncestor: false, upstreamMainAncestor: false}), 'diverged');
});

test('quality gate state requires the exact SHA and GitHub Actions success', () => {
  const sha = 'abc';
  assert.equal(qualityGateState([], sha), 'missing');
  assert.equal(qualityGateState([{id: 1, name: 'quality-gate', head_sha: sha, app: {slug: 'github-actions'}, status: 'in_progress'}], sha), 'pending');
  assert.equal(qualityGateState([{id: 1, name: 'quality-gate', head_sha: sha, app: {slug: 'github-actions'}, status: 'completed', conclusion: 'failure'}], sha), 'failure');
  assert.equal(qualityGateState([
    {id: 1, name: 'quality-gate', head_sha: 'old', app: {slug: 'github-actions'}, status: 'completed', conclusion: 'success'},
    {id: 2, name: 'quality-gate', head_sha: sha, app: {slug: 'github-actions'}, status: 'completed', conclusion: 'success'},
  ], sha), 'success');
});

test('closed unmerged PR blocks only the same source head', () => {
  const pull = {state: 'closed', merged_at: null, head: {sha: 'same'}};
  assert.equal(closedPullBlocks(pull, 'same'), true);
  assert.equal(closedPullBlocks(pull, 'new'), false);
  assert.equal(closedPullBlocks({...pull, merged_at: '2026-09-17T00:00:00Z'}, 'same'), false);
});

test('promotion workflow validator rejects trust-boundary mutations', () => {
  const workflowPath = join(root, '.github/workflows/promotion-orchestrator.yml');
  const validator = join(root, 'scripts/ci/validate-promotion-workflow.mjs');
  const run = (path) => spawnSync(process.execPath, [validator, path], {cwd: root, encoding: 'utf8'});
  assert.equal(run(workflowPath).status, 0);

  const directory = mkdtempSync(join(tmpdir(), 'aegis-promotion-workflow-'));
  try {
    const source = YAML.parse(readFileSync(workflowPath, 'utf8'));
    const mutations = [
      (workflow) => { workflow.on.pull_request = {branches: ['main']}; },
      (workflow) => { workflow.permissions.contents = 'write'; },
      (workflow) => { workflow.concurrency['cancel-in-progress'] = true; },
      (workflow) => { workflow.jobs.promote.if = '${{ always() }}'; },
      (workflow) => { workflow.jobs.promote.steps[0].with['persist-credentials'] = true; },
      (workflow) => { workflow.jobs.promote.steps[2].env.AEGIS_EXTRA_TOKEN = '${{ secrets.EXTRA_TOKEN }}'; },
    ];
    for (const [index, mutate] of mutations.entries()) {
      const copy = structuredClone(source);
      mutate(copy);
      const path = join(directory, `mutation-${index}.yml`);
      writeFileSync(path, YAML.stringify(copy));
      assert.notEqual(run(path).status, 0, `workflow mutation ${index} unexpectedly passed`);
    }
  } finally {
    rmSync(directory, {recursive: true, force: true});
  }
});
