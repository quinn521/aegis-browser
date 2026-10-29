import {pathToFileURL} from 'node:url';
import {githubApi} from './promotion-github.mjs';
import {requireGreenQuality} from './promotion-quality.mjs';

export const RELEASE_REPOSITORY = 'gcsagroup/aegis-browser';
export const RELEASE_REPOSITORY_ID = '1348409663';
const candidateTag = /^candidate\/(?:(?:ios|android)\/|macos\/(?:temporary-ad-hoc\/)?)[A-Za-z0-9][A-Za-z0-9_.-]{0,95}$/u;

export async function releasePreflight(sha, {
  environment = process.env,
  api = githubApi,
  quality = requireGreenQuality,
} = {}) {
  if (!/^[0-9a-f]{40}$/u.test(sha ?? '')) throw new Error('A full release source SHA is required');
  const candidatePush = environment.GITHUB_EVENT_NAME === 'push' &&
    environment.GITHUB_REF === `refs/tags/${environment.RELEASE_TAG}` &&
    environment.RELEASE_DESTINATION === 'github-draft' &&
    candidateTag.test(environment.RELEASE_TAG ?? '');
  const rehearsal = environment.GITHUB_ACTIONS === 'true' &&
    environment.AEGIS_RELEASE_REHEARSAL === 'true' && environment.GITHUB_REPOSITORY !== RELEASE_REPOSITORY;
  const token = environment.GH_TOKEN || environment.GITHUB_TOKEN;
  if (!token) throw new Error('GH_TOKEN is required to verify upstream release admission');
  if (rehearsal) {
    const repository = environment.GITHUB_REPOSITORY;
    if (environment.AEGIS_RELEASE_ENABLED !== 'true' || environment.GITHUB_SHA !== sha ||
        environment.AEGIS_RELEASE_TEST_REPOSITORY_ID !== environment.GITHUB_REPOSITORY_ID ||
        !['artifacts', 'testflight', 'testflight-internal', 'play-internal', 'github-draft'].includes(environment.RELEASE_DESTINATION) ||
        (!candidatePush && (!['push', 'workflow_dispatch'].includes(environment.GITHUB_EVENT_NAME) ||
          !/^refs\/heads\/codex\/fastlane-release-[A-Za-z0-9_.-]+$/u.test(environment.GITHUB_REF ?? '')))) {
      throw new Error('Fork rehearsal requires an explicitly enabled repository/branch and a testing-only destination');
    }
    const repo = await api(repository, '', {token});
    if (!repo?.fork || repo?.parent?.full_name !== RELEASE_REPOSITORY || String(repo.id) !== environment.GITHUB_REPOSITORY_ID) {
      throw new Error('Rehearsal repository must be the configured fork of upstream');
    }
    const runs = await api(repository, `/actions/workflows/release-validation.yml/runs?event=push&head_sha=${sha}&per_page=100`, {token});
    const run = (runs?.workflow_runs ?? []).filter((row) => row.head_sha === sha && /^codex\/fastlane-release-/u.test(row.head_branch ?? ''))
      .sort((a, b) => Number(b.id) - Number(a.id))[0];
    if (!run) throw new Error('Fork rehearsal needs exact-source release validation');
    const jobs = await api(repository, `/actions/runs/${run.id}/attempts/${run.run_attempt}/jobs?per_page=100`, {token});
    for (const name of ['contracts', 'ios-archive']) {
      const rows = (jobs?.jobs ?? []).filter((job) => job.name === name);
      if (rows.length !== 1 || rows[0].status !== 'completed' || rows[0].conclusion !== 'success') {
        throw new Error(`Fork rehearsal validation has not passed: ${name}`);
      }
    }
    if (candidatePush) {
      const tagged = await api(repository, `/commits/${encodeURIComponent(environment.RELEASE_TAG)}`, {token});
      if (tagged?.sha !== sha) throw new Error('Rehearsal candidate tag changed');
    }
    return {repository, sha, admission: 'fork_rehearsal_only', releaseReady: false};
  }
  if (environment.GITHUB_ACTIONS === 'true') {
    if (environment.GITHUB_REPOSITORY !== RELEASE_REPOSITORY ||
        environment.GITHUB_REPOSITORY_ID !== RELEASE_REPOSITORY_ID ||
        (!candidatePush && (environment.GITHUB_EVENT_NAME !== 'workflow_dispatch' || environment.GITHUB_REF !== 'refs/heads/main')) ||
        environment.GITHUB_SHA !== sha || environment.AEGIS_RELEASE_ENABLED !== 'true') {
      throw new Error('Release requires an enabled manual run on upstream main at this exact SHA');
    }
  }
  const repo = await api(RELEASE_REPOSITORY, '', {token});
  if (String(repo?.id) !== RELEASE_REPOSITORY_ID || repo?.default_branch !== 'main') {
    throw new Error('Upstream repository identity or default branch changed');
  }
  const assertMain = async () => {
    const main = await api(RELEASE_REPOSITORY, '/git/ref/heads/main', {token});
    if (main?.object?.sha !== sha) throw new Error('Candidate must equal current upstream main; rebind after a source change');
  };
  await assertMain();
  if (environment.RELEASE_DESTINATION === 'github-draft') {
    const platform = environment.RELEASE_PLATFORM;
    const tag = environment.RELEASE_TAG ?? '';
    if (!['ios', 'android', 'macos'].includes(platform) ||
        !tag.startsWith(`candidate/${platform}/`) ||
        !candidateTag.test(tag) || tag.includes('..')) {
      throw new Error('A platform-specific candidate tag is required');
    }
    const candidate = await api(RELEASE_REPOSITORY, `/commits/${encodeURIComponent(tag)}`, {token});
    if (candidate?.sha !== sha) throw new Error('Existing upstream candidate tag must resolve to this exact main SHA');
  }
  if (!await quality(RELEASE_REPOSITORY, 'main', sha, token, api)) {
    throw new Error('Latest exact-source upstream quality/quality-gate attempt has not succeeded');
  }
  await assertMain();
  return {repository: RELEASE_REPOSITORY, sha, admission: 'upstream_quality_verified', releaseReady: false};
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    if (process.argv.length !== 4 || process.argv[2] !== '--sha') throw new Error('Usage: release-preflight.mjs --sha <full SHA>');
    console.log(JSON.stringify(await releasePreflight(process.argv[3])));
  } catch (error) {
    console.error(error.message);
    process.exitCode = 1;
  }
}
