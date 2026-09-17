#!/usr/bin/env node

import {spawnSync} from 'node:child_process';
import {mkdtempSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {pathToFileURL} from 'node:url';

const CONVENTIONAL_TYPES = new Set([
  'build', 'chore', 'ci', 'docs', 'feat', 'fix', 'perf', 'refactor', 'release', 'revert', 'style', 'test',
]);
const CONVENTIONAL_TITLE = /^(?<type>[a-z][a-z0-9-]*)(?:\((?<scope>[^()\r\n]+)\))?(?<breaking>!)?: (?<subject>\S.*)$/u;
const PR_NUMBER_SUFFIX = /\s+\(#\d+\)$/u;
const PROMOTION_BRANCH_PREFIX = 'automation/promote-';
const README_FILES = ['README.md', 'README.zh-CN.md', 'README.zh-TW.md'];
const MARKERS = Object.freeze({
  sync: '<!-- aegis-promotion-orchestrator:main-to-develop -->',
  promotion: '<!-- aegis-promotion-orchestrator:develop-to-main -->',
  upstream: '<!-- aegis-promotion-orchestrator:upstream-main -->',
});

function command(commandName, args, {env = process.env, allowStatuses = [0]} = {}) {
  const result = spawnSync(commandName, args, {encoding: 'utf8', env});
  if (result.error) throw result.error;
  if (!allowStatuses.includes(result.status)) {
    const detail = [result.stderr, result.stdout].filter(Boolean).join('\n').trim();
    throw new Error(`${commandName} ${args.join(' ')} failed with status ${result.status}${detail ? `: ${detail}` : ''}`);
  }
  return result;
}

function git(...args) {
  return command('git', args).stdout.trim();
}

function secretEnv(token) {
  return {...process.env, GH_TOKEN: token};
}

function requireToken(name, token) {
  if (!token) throw new Error(`${name} is required for this promotion transition`);
  return token;
}

function stripPrNumberSuffixes(value) {
  let current = value;
  let previous;
  do {
    previous = current;
    current = current.replace(PR_NUMBER_SUFFIX, '').trimEnd();
  } while (current !== previous);
  return current;
}

export function parseConventionalTitle(value) {
  if (typeof value !== 'string') return null;
  const normalized = stripPrNumberSuffixes(value.trim());
  const match = CONVENTIONAL_TITLE.exec(normalized);
  if (!match?.groups || !CONVENTIONAL_TYPES.has(match.groups.type)) return null;
  return {title: normalized, type: match.groups.type};
}

export function inferPromotionTitle(subjects) {
  const candidates = (subjects ?? []).map(parseConventionalTitle).filter((entry) => entry && entry.type !== 'release');
  const feature = candidates.findLast((entry) => entry.type === 'feat');
  return feature?.title ?? candidates.at(-1)?.title ?? null;
}

export function classifyBranchRelationship({originMain, upstreamMain, originMainAncestor, upstreamMainAncestor}) {
  if (originMain === upstreamMain) return 'same';
  if (originMainAncestor && !upstreamMainAncestor) return 'origin-behind';
  if (!originMainAncestor && upstreamMainAncestor) return 'origin-ahead';
  return 'diverged';
}

export function qualityGateState(checkRuns, sha) {
  const candidates = (checkRuns ?? [])
    .filter((run) => run?.name === 'quality-gate' && run?.head_sha === sha && run?.app?.slug === 'github-actions')
    .sort((left, right) => Number(right.id ?? 0) - Number(left.id ?? 0));
  const latest = candidates[0];
  if (!latest) return 'missing';
  if (latest.status !== 'completed') return 'pending';
  return latest.conclusion === 'success' ? 'success' : 'failure';
}

export function closedPullBlocks(pr, expectedHeadSha) {
  return Boolean(pr && pr.state === 'closed' && !pr.merged_at && pr.head?.sha === expectedHeadSha);
}

async function githubApi(repo, path, {token = process.env.GH_TOKEN, method = 'GET', body} = {}) {
  const headers = {
    Accept: 'application/vnd.github+json',
    'X-GitHub-Api-Version': '2022-11-28',
    'User-Agent': 'aegis-promotion-orchestrator',
  };
  if (token) headers.Authorization = `Bearer ${token}`;
  if (body !== undefined) headers['Content-Type'] = 'application/json';
  const response = await globalThis.fetch(`https://api.github.com/repos/${repo}${path}`, {
    method,
    headers,
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const text = await response.text();
  if (!response.ok) throw new Error(`GitHub API ${method} ${repo}${path} failed: ${response.status} ${text.slice(0, 800)}`);
  return text ? JSON.parse(text) : null;
}

function repoOwner(repo) {
  const [owner] = repo.split('/');
  if (!owner) throw new Error(`Invalid repository: ${repo}`);
  return owner;
}

function ensureRemote(name, url) {
  const existing = command('git', ['remote', 'get-url', name], {allowStatuses: [0, 2, 128]});
  if (existing.status === 0) git('remote', 'set-url', name, url);
  else git('remote', 'add', name, url);
}

function refreshBranches(personalRepo, upstreamRepo) {
  ensureRemote('origin', `https://github.com/${personalRepo}.git`);
  ensureRemote('upstream', `https://github.com/${upstreamRepo}.git`);
  git('fetch', '--prune', 'origin', '+refs/heads/main:refs/remotes/origin/main', '+refs/heads/develop:refs/remotes/origin/develop');
  git('fetch', '--prune', 'upstream', '+refs/heads/main:refs/remotes/upstream/main');
}

function sha(ref) {
  return git('rev-parse', ref);
}

function isAncestor(ancestor, descendant) {
  const result = command('git', ['merge-base', '--is-ancestor', ancestor, descendant], {allowStatuses: [0, 1]});
  return result.status === 0;
}

function branchRelationship() {
  const originMain = sha('origin/main');
  const upstreamMain = sha('upstream/main');
  return {
    originMain,
    upstreamMain,
    state: classifyBranchRelationship({
      originMain,
      upstreamMain,
      originMainAncestor: isAncestor('origin/main', 'upstream/main'),
      upstreamMainAncestor: isAncestor('upstream/main', 'origin/main'),
    }),
  };
}

async function requireGreenQuality(repo, commitSha, token) {
  const result = await githubApi(repo, `/commits/${commitSha}/check-runs?check_name=quality-gate&filter=latest&per_page=100`, {token});
  const state = qualityGateState(result?.check_runs, commitSha);
  if (state === 'success') return true;
  if (state === 'missing' || state === 'pending') {
    console.log(`Deferred: ${repo}@${commitSha.slice(0, 12)} quality-gate is ${state}`);
    return false;
  }
  throw new Error(`${repo}@${commitSha.slice(0, 12)} quality-gate failed`);
}

async function listPulls(repo, state, token) {
  return githubApi(repo, `/pulls?state=${state}&per_page=100&sort=updated&direction=desc`, {token});
}

function isHead(pr, repo, branch) {
  return pr?.head?.repo?.full_name === repo && pr?.head?.ref === branch;
}

function hasMarker(pr, marker) {
  return String(pr?.body ?? '').includes(marker);
}

function candidatePersonalPull(pr, personalRepo) {
  if (pr?.base?.ref === 'develop' && isHead(pr, personalRepo, 'main')) return 'sync';
  if (pr?.base?.ref === 'main' && pr?.head?.repo?.full_name === personalRepo && String(pr?.head?.ref ?? '').startsWith(PROMOTION_BRANCH_PREFIX)) return 'promotion';
  return null;
}

function candidateUpstreamPull(pr, personalRepo) {
  return pr?.base?.ref === 'main' && isHead(pr, personalRepo, 'main');
}

function runGh(args, token) {
  return command('gh', args, {env: secretEnv(token)}).stdout.trim();
}

function ensureCopilotReview(repo, pullNumber, token) {
  runGh(['pr', 'edit', String(pullNumber), '--repo', repo, '--add-reviewer', '@copilot'], token);
  console.log(`Copilot review requested for ${repo}#${pullNumber}`);
}

function isCopilotLogin(login) {
  return String(login ?? '').toLowerCase().includes('copilot');
}

async function hasCurrentCopilotReview(repo, pull, token) {
  const [requested, reviews] = await Promise.all([
    githubApi(repo, `/pulls/${pull.number}/requested_reviewers`, {token}),
    githubApi(repo, `/pulls/${pull.number}/reviews?per_page=100`, {token}),
  ]);
  if ((requested?.users ?? []).some((user) => isCopilotLogin(user?.login))) return true;
  return (reviews ?? []).some((review) => isCopilotLogin(review?.user?.login) && review?.commit_id === pull?.head?.sha);
}

async function ensureCurrentCopilotReview(repo, pull, token) {
  if (await hasCurrentCopilotReview(repo, pull, token)) {
    console.log(`Copilot review is already requested or current for ${repo}#${pull.number}`);
    return;
  }
  ensureCopilotReview(repo, pull.number, token);
}

function ensureAutoMerge(repo, pullNumber, token) {
  runGh(['pr', 'merge', String(pullNumber), '--repo', repo, '--auto', '--merge'], token);
  console.log(`Merge-commit auto-merge enabled for ${repo}#${pullNumber}`);
}

function ensureExistingAutoMerge(repo, pull, token) {
  if (pull?.auto_merge) {
    console.log(`Merge-commit auto-merge is already enabled for ${repo}#${pull.number}`);
    return;
  }
  ensureAutoMerge(repo, pull.number, token);
}

function bodyFile(body) {
  const directory = mkdtempSync(join(tmpdir(), 'aegis-promotion-'));
  const path = join(directory, 'body.md');
  writeFileSync(path, `${body.trim()}\n`);
  return {directory, path};
}

function createPull({repo, base, head, title, body, token}) {
  const temp = bodyFile(body);
  try {
    const output = runGh(['pr', 'create', '--repo', repo, '--base', base, '--head', head, '--title', title, '--body-file', temp.path], token);
    const url = output.split(/\r?\n/u).findLast((line) => /^https:\/\//u.test(line.trim()))?.trim();
    const match = /\/pull\/(\d+)$/u.exec(url ?? '');
    if (!match) throw new Error(`Unable to parse created PR URL from gh output: ${output}`);
    return {number: Number(match[1]), html_url: url};
  } finally {
    rmSync(temp.directory, {recursive: true, force: true});
  }
}

function setupGitAuth(token) {
  command('gh', ['auth', 'setup-git'], {env: secretEnv(token)});
}

function pushOrigin(refspec, token) {
  setupGitAuth(token);
  command('git', ['push', 'origin', refspec], {env: secretEnv(token)});
}

function remoteOriginBranchSha(branch) {
  const result = command('git', ['ls-remote', '--exit-code', '--heads', 'origin', `refs/heads/${branch}`], {
    allowStatuses: [0, 2],
  });
  if (result.status === 2) return null;
  return result.stdout.trim().split(/\s+/u)[0] || null;
}

function promotionSubjects(baseRef, headRef) {
  const output = git('log', '--reverse', '--format=%s', `${baseRef}..${headRef}`);
  return output ? output.split(/\r?\n/u) : [];
}

function promotionTitle(baseRef, headRef) {
  const title = inferPromotionTitle(promotionSubjects(baseRef, headRef));
  if (!title) throw new Error(`No unambiguous Conventional Commit title found in ${baseRef}..${headRef}`);
  return title;
}

async function findLatestMatchingPull(repo, personalRepo, {base, branch}, token) {
  const pulls = await listPulls(repo, 'all', token);
  return pulls.find((pr) => pr?.base?.ref === base && isHead(pr, personalRepo, branch)) ?? null;
}

async function handleExistingPulls(personalRepo, upstreamRepo, forkToken, upstreamToken) {
  const personalOpen = await listPulls(personalRepo, 'open', forkToken);
  const personalCandidates = personalOpen
    .map((pr) => ({pr, kind: candidatePersonalPull(pr, personalRepo)}))
    .filter((entry) => entry.kind);
  const upstreamOpen = await listPulls(upstreamRepo, 'open', upstreamToken);
  const upstreamCandidates = upstreamOpen.filter((pr) => candidateUpstreamPull(pr, personalRepo));

  const manualPersonal = personalCandidates.find(({pr, kind}) => !hasMarker(pr, MARKERS[kind]));
  if (manualPersonal) {
    console.log(`Deferred: existing non-automation PR ${personalRepo}#${manualPersonal.pr.number} uses the promotion path`);
    return true;
  }
  const manualUpstream = upstreamCandidates.find((pr) => !hasMarker(pr, MARKERS.upstream));
  if (manualUpstream) {
    console.log(`Deferred: existing non-automation upstream PR ${upstreamRepo}#${manualUpstream.number} uses ${repoOwner(personalRepo)}:main`);
    return true;
  }

  const automation = [
    ...personalCandidates.filter(({pr, kind}) => hasMarker(pr, MARKERS[kind])).map(({pr, kind}) => ({repo: personalRepo, pr, kind})),
    ...upstreamCandidates.filter((pr) => hasMarker(pr, MARKERS.upstream)).map((pr) => ({repo: upstreamRepo, pr, kind: 'upstream'})),
  ];
  if (automation.length > 1) throw new Error(`Multiple promotion PRs are open: ${automation.map(({repo, pr}) => `${repo}#${pr.number}`).join(', ')}`);
  if (automation.length === 0) return false;

  const active = automation[0];
  if (active.kind === 'upstream') {
    await ensureCurrentCopilotReview(active.repo, active.pr, upstreamToken);
  } else {
    await ensureCurrentCopilotReview(active.repo, active.pr, forkToken);
    ensureExistingAutoMerge(active.repo, active.pr, forkToken);
  }
  console.log(`Deferred: waiting for ${active.kind} PR ${active.repo}#${active.pr.number}`);
  return true;
}

async function createMainToDevelopSync(personalRepo, forkToken, originMain) {
  const existing = await findLatestMatchingPull(personalRepo, personalRepo, {base: 'develop', branch: 'main'}, forkToken);
  if (closedPullBlocks(existing, originMain)) {
    throw new Error(`Previous main -> develop PR #${existing.number} was closed without merge for ${originMain.slice(0, 12)}`);
  }
  const pull = createPull({
    repo: personalRepo,
    base: 'develop',
    head: 'main',
    title: 'chore(sync): merge main into develop',
    token: forkToken,
    body: `${MARKERS.sync}\n\nSync the latest personal main into develop before the next public promotion.\n\n- source: \`${originMain}\`\n- merge method: merge commit\n- automation: serial; the next transition waits for this PR and the resulting develop push CI`,
  });
  ensureCopilotReview(personalRepo, pull.number, forkToken);
  ensureAutoMerge(personalRepo, pull.number, forkToken);
  console.log(`Created main -> develop sync PR: ${pull.html_url}`);
}

async function createDevelopToMainPromotion(personalRepo, upstreamMain, originDevelop, forkToken) {
  const shortSha = originDevelop.slice(0, 12);
  const branch = `${PROMOTION_BRANCH_PREFIX}${shortSha}`;
  const previous = await findLatestMatchingPull(personalRepo, personalRepo, {base: 'main', branch}, forkToken);
  if (previous?.state === 'closed' && !previous.merged_at) {
    throw new Error(`Previous promotion PR #${previous.number} for ${branch} was closed without merge`);
  }

  const title = promotionTitle('origin/main', 'origin/develop');
  git('switch', '--force-create', branch, 'origin/develop');
  git('restore', '--source=upstream/main', '--', ...README_FILES);
  const staged = command('git', ['diff', '--quiet', '--', ...README_FILES], {allowStatuses: [0, 1]});
  if (staged.status === 1) {
    git('add', '--', ...README_FILES);
    command('git', [
      '-c', 'user.name=github-actions[bot]',
      '-c', 'user.email=41898282+github-actions[bot]@users.noreply.github.com',
      'commit', '-m', 'chore(promotion): mirror upstream README',
    ]);
  }
  if (command('git', ['diff', '--quiet', 'origin/main', 'HEAD'], {allowStatuses: [0, 1]}).status === 0) {
    console.log('No public tree difference remains after applying the README mirror rule');
    return;
  }

  let promotionHead = sha('HEAD');
  const existingRemoteHead = remoteOriginBranchSha(branch);
  if (existingRemoteHead) {
    git('fetch', 'origin', `+refs/heads/${branch}:refs/remotes/origin/${branch}`);
    if (command('git', ['diff', '--quiet', `origin/${branch}`, 'HEAD'], {allowStatuses: [0, 1]}).status !== 0) {
      throw new Error(`Fail closed: existing ${branch} differs from the expected promotion tree`);
    }
    promotionHead = sha(`origin/${branch}`);
    console.log(`Reusing existing promotion branch ${branch}@${promotionHead.slice(0, 12)}`);
  } else {
    pushOrigin(`HEAD:refs/heads/${branch}`, forkToken);
  }
  const pull = createPull({
    repo: personalRepo,
    base: 'main',
    head: branch,
    title,
    token: forkToken,
    body: `${MARKERS.promotion}\n\nPromote the validated develop state to personal main.\n\n- develop source: \`${originDevelop}\`\n- promotion head: \`${promotionHead}\`\n- upstream README source: \`${upstreamMain}\`\n- merge method: merge commit\n- automation: serial; upstream PR creation waits for main push CI`,
  });
  ensureCopilotReview(personalRepo, pull.number, forkToken);
  ensureAutoMerge(personalRepo, pull.number, forkToken);
  console.log(`Created develop -> main promotion PR: ${pull.html_url}`);
}

async function createUpstreamPromotion(personalRepo, upstreamRepo, originMain, upstreamMain, upstreamToken) {
  const owner = repoOwner(personalRepo);
  const previous = await findLatestMatchingPull(upstreamRepo, personalRepo, {base: 'main', branch: 'main'}, upstreamToken);
  if (closedPullBlocks(previous, originMain)) {
    throw new Error(`Previous upstream PR #${previous.number} was closed without merge for ${originMain.slice(0, 12)}`);
  }
  const title = promotionTitle('upstream/main', 'origin/main');
  const pull = createPull({
    repo: upstreamRepo,
    base: 'main',
    head: `${owner}:main`,
    title,
    token: upstreamToken,
    body: `${MARKERS.upstream}\n\nPromote the validated personal main to upstream main.\n\n- upstream base: \`${upstreamMain}\`\n- personal main: \`${originMain}\`\n- automation intentionally does not enable upstream auto-merge; upstream review and merge policy remain authoritative`,
  });
  ensureCopilotReview(upstreamRepo, pull.number, upstreamToken);
  console.log(`Created upstream promotion PR: ${pull.html_url}`);
}

async function main() {
  const personalRepo = process.env.GITHUB_REPOSITORY;
  const upstreamRepo = process.env.AEGIS_UPSTREAM_REPOSITORY || 'gcsagroup/aegis-browser';
  const readToken = process.env.GH_TOKEN;
  const forkToken = requireToken('AEGIS_FORK_AUTOMATION_TOKEN', process.env.AEGIS_FORK_AUTOMATION_TOKEN);
  const upstreamToken = requireToken('AEGIS_UPSTREAM_TOKEN', process.env.AEGIS_UPSTREAM_TOKEN);
  if (!personalRepo || !readToken) throw new Error('GITHUB_REPOSITORY and GH_TOKEN are required');

  refreshBranches(personalRepo, upstreamRepo);
  if (await handleExistingPulls(personalRepo, upstreamRepo, forkToken, upstreamToken)) return;

  let relation = branchRelationship();

  if (relation.state === 'diverged') {
    throw new Error(`Fail closed: personal main ${relation.originMain.slice(0, 12)} and upstream main ${relation.upstreamMain.slice(0, 12)} diverged`);
  }

  if (relation.state === 'origin-behind') {
    if (!await requireGreenQuality(upstreamRepo, relation.upstreamMain, upstreamToken)) return;
    refreshBranches(personalRepo, upstreamRepo);
    const refreshed = branchRelationship();
    if (refreshed.state !== 'origin-behind' || refreshed.upstreamMain !== relation.upstreamMain || refreshed.originMain !== relation.originMain) {
      console.log('Deferred: main relationship changed after upstream quality verification');
      return;
    }
    pushOrigin(`${relation.upstreamMain}:refs/heads/main`, forkToken);
    console.log(`Fast-forwarded personal main to upstream ${relation.upstreamMain}`);
    return;
  }

  if (relation.state === 'origin-ahead') {
    if (!await requireGreenQuality(personalRepo, relation.originMain, readToken)) return;
    refreshBranches(personalRepo, upstreamRepo);
    const refreshed = branchRelationship();
    if (refreshed.state !== 'origin-ahead' || refreshed.originMain !== relation.originMain || refreshed.upstreamMain !== relation.upstreamMain) {
      console.log('Deferred: main relationship changed after personal main quality verification');
      return;
    }
    await createUpstreamPromotion(personalRepo, upstreamRepo, refreshed.originMain, refreshed.upstreamMain, upstreamToken);
    return;
  }

  const verifiedMain = relation.originMain;
  if (!await requireGreenQuality(personalRepo, verifiedMain, readToken)) return;
  refreshBranches(personalRepo, upstreamRepo);
  relation = branchRelationship();
  if (relation.state !== 'same' || relation.originMain !== verifiedMain) {
    console.log(`Deferred: main relationship changed after personal main quality verification (${relation.state})`);
    return;
  }
  if (!isAncestor('origin/main', 'origin/develop')) {
    await createMainToDevelopSync(personalRepo, forkToken, relation.originMain);
    return;
  }

  let currentDevelop = sha('origin/develop');
  if (currentDevelop === relation.originMain) {
    console.log('No develop changes are waiting for promotion');
    return;
  }
  if (!await requireGreenQuality(personalRepo, currentDevelop, readToken)) return;
  const verifiedDevelop = currentDevelop;
  refreshBranches(personalRepo, upstreamRepo);
  relation = branchRelationship();
  currentDevelop = sha('origin/develop');
  if (relation.state !== 'same' || currentDevelop !== verifiedDevelop) {
    console.log(`Deferred: branch state changed during develop quality verification (${relation.state})`);
    return;
  }
  await createDevelopToMainPromotion(personalRepo, relation.upstreamMain, currentDevelop, forkToken);
}

const invoked = process.argv[1] && pathToFileURL(resolve(process.argv[1])).href === import.meta.url;
if (invoked) {
  main().catch((error) => {
    console.error(error instanceof Error ? error.message : String(error));
    process.exitCode = 1;
  });
}
