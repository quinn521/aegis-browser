import assert from 'node:assert/strict';
import test from 'node:test';
import {releasePreflight, RELEASE_REPOSITORY, RELEASE_REPOSITORY_ID} from '../release-preflight.mjs';

const sha = 'a'.repeat(40);
const environment = {
  GH_TOKEN: 'fixture-token', GITHUB_ACTIONS: 'true', GITHUB_REPOSITORY: RELEASE_REPOSITORY,
  GITHUB_REPOSITORY_ID: RELEASE_REPOSITORY_ID, GITHUB_EVENT_NAME: 'workflow_dispatch',
  GITHUB_REF: 'refs/heads/main', GITHUB_SHA: sha, AEGIS_RELEASE_ENABLED: 'true',
};
function options(overrides = {}) {
  return {
    environment,
    api: async (repo, path) => {
      assert.equal(repo, RELEASE_REPOSITORY);
      return path === '' ? {id: Number(RELEASE_REPOSITORY_ID), default_branch: 'main'} : {object: {sha}};
    },
    quality: async (repo, branch, actual) => {
      assert.deepEqual([repo, branch, actual], [RELEASE_REPOSITORY, 'main', sha]);
      return true;
    },
    ...overrides,
  };
}

test('release requires exact upstream identity and returns no distribution qualification', async () => {
  const result = await releasePreflight(sha, options());
  assert.equal(result.admission, 'upstream_quality_verified');
  assert.equal(result.releaseReady, false);
});

for (const [key, value] of Object.entries({
  GITHUB_REPOSITORY: 'quinn521/aegis-browser', GITHUB_REPOSITORY_ID: '0',
  GITHUB_EVENT_NAME: 'pull_request', GITHUB_REF: 'refs/heads/develop',
  GITHUB_SHA: 'b'.repeat(40), AEGIS_RELEASE_ENABLED: 'false',
})) {
  test(`release refuses ${key} mismatch before API access`, async () => {
    await assert.rejects(releasePreflight(sha, options({environment: {...environment, [key]: value},
      api: () => assert.fail('must reject before API calls')})), /enabled manual run/u);
  });
}

test('missing, pending or failed exact-source quality blocks release', async () => {
  await assert.rejects(releasePreflight(sha, options({quality: async () => false})), /has not succeeded/u);
  await assert.rejects(releasePreflight(sha, options({quality: async () => { throw new Error('failed attempt'); }})), /failed attempt/u);
});

test('main advancing while quality is read invalidates admission', async () => {
  let reads = 0;
  const api = async (repo, path) => path === ''
    ? {id: Number(RELEASE_REPOSITORY_ID), default_branch: 'main'}
    : {object: {sha: ++reads === 1 ? sha : 'b'.repeat(40)}};
  await assert.rejects(releasePreflight(sha, options({api})), /current upstream main/u);
});

test('local upload still requires upstream CI and a read token', async () => {
  await assert.rejects(releasePreflight(sha, options({environment: {}})), /GH_TOKEN/u);
  await assert.rejects(releasePreflight('main', options()), /full release source SHA/u);
});

test('candidate tag is checked before starting expensive release builds', async () => {
  const env = {...environment, RELEASE_DESTINATION: 'github-draft', RELEASE_PLATFORM: 'android', RELEASE_TAG: 'candidate/android/2.2.0.144'};
  const base = options();
  const api = async (repo, path, opts) => path.startsWith('/commits/') ? {sha} : base.api(repo, path, opts);
  assert.equal((await releasePreflight(sha, options({environment: env, api}))).sha, sha);
  await assert.rejects(releasePreflight(sha, options({environment: {...env, RELEASE_TAG: 'v2.2.0.144'}, api})), /platform-specific/u);
  await assert.rejects(releasePreflight(sha, options({environment: env,
    api: async (repo, path, opts) => path.startsWith('/commits/') ? {sha: 'b'.repeat(40)} : base.api(repo, path, opts)})), /candidate tag must/u);
});

test('automatic candidate tags still require the exact current upstream main and quality', async () => {
  const tag = 'candidate/ios/0.8.0-alpha.1';
  const env = {...environment, GITHUB_EVENT_NAME: 'push', GITHUB_REF: `refs/tags/${tag}`,
    RELEASE_DESTINATION: 'github-draft', RELEASE_PLATFORM: 'ios', RELEASE_TAG: tag};
  const base = options();
  const api = async (repo, path, opts) => path.startsWith('/commits/') ? {sha} : base.api(repo, path, opts);
  assert.equal((await releasePreflight(sha, options({environment: env, api}))).admission, 'upstream_quality_verified');
  await assert.rejects(releasePreflight(sha, options({environment: {...env, RELEASE_DESTINATION: 'app-store'}, api})), /enabled manual run/u);
  await assert.rejects(releasePreflight(sha, options({environment: env, api, quality: async()=>false})), /has not succeeded/u);
});

const forkEnvironment = {...environment, GITHUB_REPOSITORY: 'fixture/aegis-browser', GITHUB_REPOSITORY_ID: '777',
  AEGIS_RELEASE_TEST_REPOSITORY_ID: '777', AEGIS_RELEASE_REHEARSAL: 'true',
  GITHUB_EVENT_NAME: 'push', GITHUB_REF: 'refs/heads/codex/fastlane-release-test', RELEASE_DESTINATION: 'artifacts'};

function forkApi({conclusion = 'success', parent = RELEASE_REPOSITORY, runSha = sha} = {}) {
  return async (repo, path) => {
    assert.equal(repo, 'fixture/aegis-browser');
    if (path === '') return {id: 777, fork: true, parent: {full_name: parent}};
    if (path.startsWith('/actions/workflows/')) return {workflow_runs: [{id: 5, run_attempt: 2, head_sha: runSha, head_branch: 'codex/fastlane-release-test'}]};
    if (path.includes('/attempts/2/jobs')) return {jobs: ['contracts', 'ios-archive'].map(name=>({name, status:'completed', conclusion}))};
    if (path.startsWith('/commits/')) return {sha};
    assert.fail(`Unexpected API request ${path}`);
  };
}

test('explicit fork rehearsal admits only tested source and records that it is not upstream qualification', async () => {
  const result = await releasePreflight(sha, options({environment:forkEnvironment, api:forkApi(), quality:()=>assert.fail('upstream quality is separate')}));
  assert.equal(result.admission, 'fork_rehearsal_only');
  assert.equal(result.releaseReady, false);
  for (const mismatch of [{conclusion:'failure'}, {parent:'elsewhere/repo'}, {runSha:'b'.repeat(40)}]) {
    await assert.rejects(releasePreflight(sha, options({environment:forkEnvironment, api:forkApi(mismatch)})));
  }
});

test('fork rehearsal rejects production, external testing, mismatched repository and untrusted refs before API access', async () => {
  const changes = [{RELEASE_DESTINATION:'app-store'}, {RELEASE_DESTINATION:'play-production'},
    {RELEASE_DESTINATION:'testflight-external'}, {AEGIS_RELEASE_TEST_REPOSITORY_ID:'888'},
    {GITHUB_EVENT_NAME:'pull_request'}, {GITHUB_REF:'refs/heads/main'}];
  for (const change of changes) {
    await assert.rejects(releasePreflight(sha, options({environment:{...forkEnvironment,...change}, api:()=>assert.fail('no API access')})), /Fork rehearsal requires/u);
  }
});
