// Replay the shipped Android patches, then typecheck and exercise their real UI.
import assert from 'node:assert/strict';
import {execFileSync} from 'node:child_process';
import {mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {createRequire} from 'node:module';
import {tmpdir} from 'node:os';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import vm from 'node:vm';
import {JSDOM} from 'jsdom';

const browser = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const patches = resolve(browser, 'patches-android');
const ts = createRequire(new URL('../../../packages/core/package.json', import.meta.url))('typescript');
const scratch = mkdtempSync(resolve(tmpdir(), 'aegis-android-gestures-'));
try {
  execFileSync('git', ['init', '-q', scratch]);
  let applied = 0;
  for (const name of readFileSync(resolve(patches, 'series'), 'utf8').split('\n')) {
    if (!name.trim() || name.startsWith('#')) continue;
    const patch = resolve(patches, name.trim());
    if (!readFileSync(patch, 'utf8').includes('diff --git a/chrome/browser/resources/aegis/gestures.')) continue;
    execFileSync('git', ['apply', '--include=chrome/browser/resources/aegis/gestures.*', patch], {cwd: scratch});
    applied++;
  }
  assert.ok(applied > 0, 'Exercise actual Android gesture patches');
  const base = resolve(scratch, 'chrome/browser/resources/aegis');
  const declarations = resolve(scratch, 'webui.d.ts');
  writeFileSync(declarations, `declare module 'chrome://resources/js/cr.js' {
    export function sendWithPromise<T>(...args: unknown[]): Promise<T>;
  }
  declare namespace chrome { function send(message: string, args?: unknown[]): void; }\n`);
  const program = ts.createProgram([resolve(base, 'gestures.ts'), declarations], {
    target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ESNext,
    moduleResolution: ts.ModuleResolutionKind.Bundler,
    lib: ['lib.es2022.d.ts', 'lib.dom.d.ts', 'lib.dom.iterable.d.ts'],
    strict: true, skipLibCheck: true, noEmit: true, types: [],
  });
  const diagnostics = ts.getPreEmitDiagnostics(program);
  assert.equal(diagnostics.length, 0, ts.formatDiagnostics(diagnostics, {
    getCurrentDirectory: () => base, getCanonicalFileName: name => name, getNewLine: () => '\n',
  }));
  const code = ts.transpileModule(readFileSync(resolve(base, 'gestures.ts'), 'utf8'), {
    compilerOptions: {target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS},
  }).outputText;
  function harness(missingLine = false) {
    const dom = new JSDOM(readFileSync(resolve(base, 'gestures.html'), 'utf8'));
    const {window} = dom;
    const {document} = window;
    const practice = document.getElementById('practice');
    practice.setPointerCapture = () => {};
    practice.getBoundingClientRect = () => ({left: 0, top: 0, right: 700, bottom: 300});
    if (missingLine) document.getElementById('practiceLine').remove();
    const calls = [];
    vm.runInNewContext(code, {
      exports: {}, document, window, Error, Option: window.Option,
      innerWidth: 1000, innerHeight: 800, requestAnimationFrame: callback => callback(),
      ResizeObserver: class {observe() {}}, chrome: {send: (...args) => calls.push(args)},
      require: () => ({sendWithPromise: async () => ({
        settings: {enabled: true, showTrail: true, threshold: 24, bindings: {L: 'back'}, disabledSites: []},
        actions: [{id: 'back', label: '后退', pdfOnly: false}],
      })}),
    });
    return {dom, window, document, practice, calls};
  }
  const h = harness();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(h.document.getElementById('status').textContent, '设置已加载');
  const event = (type, x) => h.practice.dispatchEvent(new h.window.MouseEvent(type, {
    button: 2, clientX: x, clientY: 80, bubbles: true, cancelable: true,
  }));
  event('pointerdown', 120);
  assert.notEqual(h.document.getElementById('practiceLine').getAttribute('points'), '');
  event('pointermove', 40);
  event('pointerup', 40);
  assert.equal(h.document.getElementById('practiceResult').textContent, '匹配：后退（练习不执行）');
  assert.ok(h.calls.every(([name]) => name === 'aegisGesturePracticeRegion'));
  h.window.close();
  assert.throws(() => harness(true), /缺少设置元素：practiceLine/);
  console.log('Android gesture patch replay, strict typing, pointer practice and missing-element regression passed');
} finally {
  rmSync(scratch, {recursive: true, force: true});
}
