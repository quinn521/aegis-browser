import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {createRequire} from 'node:module';
import {JSDOM} from 'jsdom';
import {downloadBindings, assertDownloadDispatch} from './downloads-template-bindings.mjs';
const ts = createRequire(new URL('../../../../packages/core/package.json', import.meta.url))('typescript');
const source = fs.readFileSync(new URL('../../overlay/chrome/browser/resources/settings/downloads_page/downloads_page.html.ts', import.meta.url), 'utf8');
test('wrong receiver, binding, missing and extra callbacks are rejected', () => {
  assert.equal(downloadBindings(ts, source).length, 10);
  for (const [name, changed] of [
    ['receiver', source.replace('this.onAegisRetryClick_', 'other.onAegisRetryClick_')],
    ['eager call', source.replace('this.onAegisRetryClick_', 'this.onAegisRetryClick_()')],
    ['inline arrow', source.replace('this.onAegisRetryClick_', '() => this.onAegisRetryClick_()')],
    ['missing', source.replace('@click="${this.onAegisRetryClick_}"', '')],
    ['extra', source.replace('@click="${this.onAegisRetryClick_}"', '@click="${this.onAegisRetryClick_}" @click="${this.onAegisRetryClick_}"')],
    ['wrong target', source.replace('this.onAegisRetryClick_', 'this.onUnknown_')],
    ['wrong control', source.replace('<cr-toggle id="aegisReduceOnMetered"', '<cr-input id="aegisReduceOnMetered"')],
    ['wrong control id', source.replace('id="aegisReduceOnMetered"', 'id="aegisUnknown"')],
    ['wrong event', source.replace('@click="${this.onAegisRetryClick_}"', '@change="${this.onAegisRetryClick_}"')],
  ]) assert.throws(() => downloadBindings(ts, changed), undefined, name);
});
test('dispatch rejects control receiver, missing event and incorrect control', () => {
  const dom = new JSDOM();
  try {
    const control = dom.window.document.createElement('select');
    control.append(new dom.window.Option('custom', 'custom'));
    const owner = {};
    let event, calls;
    control.addEventListener('change', actual => {
      event = actual;
      calls = [{owner, event, target: event.currentTarget, value: event.currentTarget.value}];
      assertDownloadDispatch(owner, control, event, calls);
    });
    control.dispatchEvent(new dom.window.Event('change'));
    assert.throws(() => assertDownloadDispatch(owner, control, event, [{...calls[0], owner: control}]));
    assert.throws(() => assertDownloadDispatch(owner, control, event, [{...calls[0], event: undefined}]));
    assert.throws(() => assertDownloadDispatch(owner, control, event, [{...calls[0], target: owner}]));
    assert.throws(() => assertDownloadDispatch(owner, control, event, []));
    assert.throws(() => assertDownloadDispatch(owner, control, event, [...calls, ...calls]));
  } finally { dom.window.close(); }
});
