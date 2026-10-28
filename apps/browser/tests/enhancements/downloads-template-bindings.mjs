import assert from 'node:assert/strict';

// Unit contract for Lit's host receiver. Native15 owns the separate real Lit proof.
export function downloadBindings(ts, source) {
  const tree = ts.createSourceFile('downloads.html.ts', source, ts.ScriptTarget.Latest, true);
  assert.equal(tree.parseDiagnostics.length, 0, 'template must parse');
  const bindings = [];
  function visit(node) {
    if (ts.isTemplateExpression(node)) {
      let previous = node.head.text;
      for (const span of node.templateSpans) {
        const event = /@(click|change)\s*=\s*["']$/.exec(previous);
        if (event) {
          const ref = span.expression;
          assert.ok(ts.isPropertyAccessExpression(ref) && ref.expression.kind === ts.SyntaxKind.ThisKeyword,
            'event binding must be a stable method reference on this');
          const prefix = source.slice(source.lastIndexOf('<', ref.getStart(tree)), ref.getStart(tree));
          const tag = /^<([a-z][\w-]*)\b/.exec(prefix)?.[1];
          const id = /\bid="([^"]+)"/.exec(prefix)?.[1] ?? '';
          bindings.push({type: event[1], method: ref.name.text, position: ref.getStart(tree), tag, id});
        }
        previous = span.literal.text;
      }
    }
    ts.forEachChild(node, visit);
  }
  visit(tree);
  bindings.sort((a, b) => a.position - b.position);
  const expected = [
    ['click', 'onChangeDownloadsPathClick_'], ['click', 'onClearAutoOpenFileTypesClick_'],
    ['click', 'onAegisRetryClick_'], ['change', 'onAegisParallelModeChange_'],
    ...Array.from({length: 4}, () => ['change', 'onAegisToggleChange_']),
    ['change', 'onAegisLimitModeChange_'], ['change', 'onAegisLimitChange_'],
  ];
  assert.equal(bindings.length, 10, 'all ten syntactic event bindings');
  assert.equal(new Set(bindings.map(b => b.method)).size, 7, 'all seven method targets');
  assert.deepEqual(bindings.map(b => [b.type, b.method]), expected, 'exact ordered event-to-method mapping');
  assert.deepEqual(bindings.map(b => [b.tag, b.id]), [
    ['controlled-button', 'changeDownloadsPath'], ['cr-button', 'resetAutoOpenFileTypes'],
    ['cr-button', ''], ['select', ''],
    ['cr-toggle', 'aegisReduceOnMetered'], ['cr-toggle', 'aegisReduceOnBattery'],
    ['cr-toggle', 'aegisTorrentDhtEnabled'], ['cr-toggle', 'aegisTorrentPexEnabled'],
    ['select', 'aegis${kind}Mode'], ['cr-input', 'aegisTorrent${kind}Limit'],
  ], 'each callback stays on its exact real control');
  return bindings;
}

export function assertDownloadDispatch(owner, control, event, calls) {
  assert.equal(calls.length, 1, 'exactly one callback');
  assert.equal(calls[0].owner, owner, 'Lit component receiver');
  assert.equal(calls[0].event, event, 'same actual event');
  assert.equal(calls[0].target, control, 'actual currentTarget');
  assert.equal(calls[0].value, control.value, 'actual control value');
}
