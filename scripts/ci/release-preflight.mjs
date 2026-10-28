import {pathToFileURL} from 'node:url';
import {githubApi} from './promotion-github.mjs';
import {requireGreenQuality} from './promotion-quality.mjs';

export const RELEASE_REPOSITORY = 'gcsagroup/aegis-browser';
export const RELEASE_REPOSITORY_ID = '1348409663';

export async function releasePreflight(sha, {
  environment = process.env,
  api = githubApi,
  quality = requireGreenQuality,
} = {}) {
  if (!/^[0-9a-f]{40}$/u.test(sha ?? '')) throw new Error('A full release source SHA is required');
  if (environment.GITHUB_ACTIONS === 'true') {
    if (environment.GITHUB_REPOSITORY !== RELEASE_REPOSITORY ||
        environment.GITHUB_REPOSITORY_ID !== RELEASE_REPOSITORY_ID ||
        environment.GITHUB_EVENT_NAME !== 'workflow_dispatch' ||
        environment.GITHUB_REF !== 'refs/heads/main' ||
        environment.GITHUB_SHA !== sha || environment.AEGIS_RELEASE_ENABLED !== 'true') {
      throw new Error('Release requires an enabled manual run on upstream main at this exact SHA');
    }
  }
  const token = environment.GH_TOKEN || environment.GITHUB_TOKEN;
  if (!token) throw new Error('GH_TOKEN is required to verify upstream release admission');
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
        !/^candidate\/(?:ios|android|macos)\/[A-Za-z0-9][A-Za-z0-9_.-]{0,95}$/u.test(tag) || tag.includes('..')) {
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
