// node tests/exclude.test.js src/page/console.html
// Pulls the exclusion functions out of console.html (lsconsole2 exclude) and checks them.
const fs = require('fs');
const html = fs.readFileSync(process.argv[2], 'utf8');
function grab(name) {
  const start = html.indexOf('  function ' + name + '(');
  if (start < 0) throw new Error('missing ' + name);
  const end = html.indexOf('\n  }\n', start);
  return html.slice(start, end + 4);
}
eval(html.match(/  var excludes = .*;/)[0]);
eval(['has', 'extend', 'leftOut', 'swapped', 'dressed'].map(grab).join('\n'));

// As the page keeps them: longest first.
excludes = [{ s: 'Stormfall', r: 'Kaelith' }, { s: 'Secret', r: null }, { s: 'Storm', r: 'Lake' }]
  .sort((a, b) => b.s.length - a.s.length);

let failed = 0;
function check(label, got, want) {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  if (!ok) failed++;
  console.log((ok ? 'ok   ' : 'FAIL ') + label + (ok ? '' : '  got ' + JSON.stringify(got) + ' want ' + JSON.stringify(want)));
}

check('left out, any case', leftOut('mySECRETvar'), true);
check('not left out', leftOut('Stormfall_Bags'), false);
check('swap, any case, every place', swapped('stormfall and STORMFALL'), 'Kaelith and Kaelith');
check('longest first', swapped('Stormfall by the Storm'), 'Kaelith by the Lake');
check('no match unchanged', swapped('echo'), 'echo');

const v1 = dressed({ v: 'Stormfall_Bags', k: 'variable', val: '3' });
check('name shown swapped, inserted as is', [v1.v, v1.d, v1.val], ['Stormfall_Bags', 'Kaelith_Bags', '3']);
const v2 = dressed({ v: 'Name', k: 'member', val: 'Stormfall' });
check('value swapped', [v2.d, v2.val], [undefined, 'Kaelith']);
const v3 = dressed({ v: 'Guild', k: 'member', val: 'The Secret Order' });
check('value preview dropped', v3.val, undefined);
const v4 = dressed({ v: 'myalias', k: 'alias', n: 'run secret/thing' });
check('alias target dropped', v4.n, undefined);
const v5 = dressed({ v: 'echo', k: 'command' });
check('untouched item', [v5.d, v5.val, v5.n], [undefined, undefined, undefined]);

if (failed) { console.log(failed + ' failed'); process.exit(1); }
console.log('all passed');
