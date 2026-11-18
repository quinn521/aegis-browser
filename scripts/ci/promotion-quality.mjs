import {URLSearchParams} from 'node:url';
import {githubApi} from './promotion-github.mjs';

const QUALITY = {name: 'quality', jobs: ['quality', 'quality-gate']};
const SECRET_SCANNING = {name: 'secret-scanning', jobs: ['gitleaks']};

function matchesPush(run, {repo, branch, sha}, workflow) {
  return run?.event === 'push' && run?.path === `.github/workflows/${workflow.name}.yml` &&
    run?.head_branch === branch && run?.head_sha === sha &&
    run?.repository?.full_name === repo;
}

function workflowRunState(run, expected, workflow) {
  if (!run) return 'missing';
  if (!matchesPush(run, expected, workflow)) return 'failure';
  if (run.status !== 'completed') return 'pending';
  return run.conclusion === 'success' ? 'success' : 'failure';
}

function requiredJobsState(jobs, run, expected, workflow) {
  for (const name of workflow.jobs) {
    const matches = (jobs ?? []).filter((job) => job?.name === name);
    if (matches.length !== 1) return 'failure';
    const job = matches[0];
    if (job.run_id !== run.id || job.head_sha !== expected.sha ||
        job.run_attempt !== run.run_attempt) return 'failure';
    if (job.status !== 'completed') return 'pending';
    if (job.conclusion !== 'success') return 'failure';
  }
  return 'success';
}

function workflowState(run, jobs, expected, workflow) {
  const runState = workflowRunState(run, expected, workflow);
  return runState === 'success'
    ? requiredJobsState(jobs, run, expected, workflow)
    : runState;
}

export function qualityWorkflowRunState(run, jobs, expected) {
  return workflowState(run, jobs, expected, QUALITY);
}

async function verifiedWorkflowState(run, expected, workflow, token, apiFn) {
  const state = workflowRunState(run, expected, workflow);
  if (state !== 'success') return state;
  const attempt = run.run_attempt;
  if (!Number.isInteger(attempt) || attempt < 1) return 'failure';
  const {repo} = expected;
  const result = await apiFn(repo, `/actions/runs/${run.id}/attempts/${attempt}/jobs?per_page=100`, {token});
  const final = await apiFn(repo, `/actions/runs/${run.id}`, {token});
  if (final?.id !== run.id) return 'failure';
  if (final.run_attempt !== attempt) return 'pending';
  return workflowState(final, result?.jobs, expected, workflow);
}

async function requireSuccessfulPush(expected, workflow, token, apiFn) {
  const {repo, branch, sha} = expected;
  const query = new URLSearchParams({branch, event: 'push', head_sha: sha, per_page: '100'});
  const result = await apiFn(repo, `/actions/workflows/${workflow.name}.yml/runs?${query}`, {token});
  const selected = (result?.workflow_runs ?? [])
    .filter(run => matchesPush(run, expected, workflow))
    .sort((left, right) => Number(right.id) - Number(left.id))[0];
  let state = 'missing';
  if (selected) {
    const run = await apiFn(repo, `/actions/runs/${selected.id}`, {token});
    state = run?.id === selected.id
      ? await verifiedWorkflowState(run, expected, workflow, token, apiFn)
      : 'failure';
  }
  if (state === 'success') return true;
  if (state === 'missing' || state === 'pending') {
    console.log(`Deferred: ${repo}:${branch}@${sha.slice(0, 12)} ${workflow.name} workflow is ${state}`);
    return false;
  }
  throw new Error(`${repo}:${branch}@${sha.slice(0, 12)} ${workflow.name} workflow failed`);
}

export async function requireGreenQuality(repo, branch, commitSha, token, apiFn = githubApi) {
  const expected = {repo, branch, sha: commitSha};
  return await requireSuccessfulPush(expected, QUALITY, token, apiFn) &&
    await requireSuccessfulPush(expected, SECRET_SCANNING, token, apiFn);
}
