import {execFileSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

function hashInput(root, file) {
  const input = path.join(root, file);
  const resolved = fs.realpathSync(input);
  const relative = path.relative(root, resolved);
  if (!relative || relative === '..' || relative.startsWith(`..${path.sep}`) || path.isAbsolute(relative)) {
    throw new Error(`Simulator input is outside the repository: ${file}`);
  }
  if (resolved !== input || !fs.lstatSync(input).isFile()) {
    throw new Error(`Simulator input must be a regular file: ${file}`);
  }
  const fd = fs.openSync(input, fs.constants.O_RDONLY | fs.constants.O_NOFOLLOW | fs.constants.O_NONBLOCK);
  try {
    if (!fs.fstatSync(fd).isFile()) throw new Error(`Simulator input must be a regular file: ${file}`);
    return createHash('sha256').update(fs.readFileSync(fd)).digest('hex');
  } finally {
    fs.closeSync(fd);
  }
}

export function writeInputManifest(repoRoot, destination) {
  const root = fs.realpathSync(repoRoot);
  // Include shared test resources and policy-generation inputs outside apps/ios.
  const output = execFileSync('git', [
    'ls-files', '-z', '--cached', '--others', '--exclude-standard', '--',
    'apps/ios', 'packages/core',
    'apps/browser/overlay/chrome/browser/resources/aegis/enhancements_shared',
  ], {cwd: root, maxBuffer: 16 * 1024 * 1024}).toString('utf8');
  const files = [...new Set(output.split('\0').filter(Boolean))]
    .sort((left, right) => Buffer.from(left).compare(Buffer.from(right)));
  const lines = files.map(file => {
    const digest = hashInput(root, file);
    // Match shasum's escaped filename convention, keeping each record on one line.
    const escaped = file.replaceAll('\\', '\\\\').replaceAll('\n', '\\n').replaceAll('\r', '\\r');
    return `${escaped === file ? '' : '\\'}${digest}  ${escaped}\n`;
  });
  fs.writeFileSync(destination, lines.join(''));
}

if (process.argv[1] && fileURLToPath(import.meta.url) === path.resolve(process.argv[1])) {
  if (process.argv.length !== 4) throw new Error('usage: simulator-input-manifest.mjs REPO_ROOT OUTPUT');
  writeInputManifest(process.argv[2], process.argv[3]);
}
