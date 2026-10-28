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


test('real id cannot be replaced by lookalike attributes or quoted text', t => {
  const original = 'id="aegisReduceOnMetered"';
  for (const [name, replacement] of [
    ['data-id only', 'data-id="aegisReduceOnMetered"'],
    ['aria-id only', 'aria-id="aegisReduceOnMetered"'],
    ['prefixed id only', 'other-id="aegisReduceOnMetered"'],
    ['missing id', ''],
    ['empty id', 'id=""'],
    ['boolean id', 'id'],
    ['double-quoted fake id in another value', `title='id="aegisReduceOnMetered"'`],
    ['single-quoted fake id in another value', `title="id='aegisReduceOnMetered'"`],
    ['fake tag and id inside quoted value', `title='<cr-toggle id="aegisReduceOnMetered">'`],
    ['data-id plus quoted fake id', `data-id="aegisReduceOnMetered" title=' id="aegisReduceOnMetered"'`],
    ['wrong real id plus matching data-id', 'data-id="aegisReduceOnMetered" id="aegisWrong"'],
  ]) {
    const changed = source.replace(original, replacement);
    assert.notEqual(changed, source, name);
    assert.throws(() => downloadBindings(ts, changed), undefined, name);
    t.diagnostic(name);
  }
});

test('real id survives unrelated attributes, quoting and attribute order', t => {
  const original = 'id="aegisReduceOnMetered"';
  for (const [name, replacement] of [
    ['data-id before real id', 'data-id="unrelated" id="aegisReduceOnMetered"'],
    ['data-id after real id', 'id="aegisReduceOnMetered" data-id="unrelated"'],
    ['aria-id before real id', 'aria-id="unrelated" id="aegisReduceOnMetered"'],
    ['single quotes', "id='aegisReduceOnMetered'"],
    ['unquoted value', 'id=aegisReduceOnMetered'],
    ['whitespace around equals', 'id \n = \t "aegisReduceOnMetered"'],
    ['quoted fake id before real id', `title='id="unrelated"' id="aegisReduceOnMetered"`],
    ['quoted tag before real id', `title='<cr-input id="unrelated">' id="aegisReduceOnMetered"`],
  ]) {
    const changed = source.replace(original, replacement);
    assert.notEqual(changed, source, name);
    const bindings = downloadBindings(ts, changed);
    assert.equal(bindings.length, 10, name);
    assert.equal(bindings[4].id, 'aegisReduceOnMetered', name);
    t.diagnostic(name);
  }
});
