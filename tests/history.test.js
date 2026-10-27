// node tests/history.test.js src/page/console.html
// Pulls historyQuery/words/matches out of console.html and checks the user's examples.
const fs = require('fs');
const html = fs.readFileSync(process.argv[2], 'utf8');
function grab(name) {
  const start = html.indexOf('  function ' + name + '(');
  const end = html.indexOf('\n  }\n', start);
  return html.slice(start, end + 4);
}
eval(grab('historyQuery') + grab('words') + grab('matchesAll'));

const history = [
  'echo asdf', 'run asdf xyz', 'echo xyz then asdf', 'echo ASDF XYZ together', 'lsconsole2 status', 'hist old entry',
];
function filter(text) {
  const t = historyQuery(text);
  return t === null ? null : history.filter(h => matchesAll(h, t));
}
const cases = [
  ['hi', null],
  ['his', null],
  ['hist', history],
  ['HISTORY', history],
  ['historyx', null],
  ['hist asdf', ['echo asdf', 'run asdf xyz', 'echo xyz then asdf', 'echo ASDF XYZ together']],
  ['hist asdf xyz', ['run asdf xyz', 'echo xyz then asdf', 'echo ASDF XYZ together']],
  ['hist "asdf xyz"', ['run asdf xyz', 'echo ASDF XYZ together']],
  ['histo "asdf xyz', ['run asdf xyz', 'echo ASDF XYZ together']],
  ['hist ""', history],
  ['  hist   status  ', ['lsconsole2 status']],
  ['echo hist', null],
];
let bad = 0;
for (const [text, want] of cases) {
  const got = filter(text);
  const ok = JSON.stringify(got) === JSON.stringify(want);
  if (!ok) bad++;
  console.log((ok ? 'ok   ' : 'FAIL ') + JSON.stringify(text) + (ok ? '' : ' -> ' + JSON.stringify(got)));
}

// Up/Down: this session's commands only, newest first, stopping at the oldest (the user, 2026-10-27).
eval(grab('sessionAdd') + grab('recallStep'));
const mine = [];
['echo a', 'echo b', 'echo a', 'echo c'].forEach(c => sessionAdd(mine, c));
function keys(list, seq) {
  let pos = -1, value = '';
  const seen = [];
  for (const k of seq) {
    const s = recallStep(list, pos, k === 'up' ? -1 : 1);
    if (s) { pos = s.pos; value = s.value; }
    seen.push(s ? value : '(nothing)');
  }
  return seen;
}
const recall = [
  ['a repeat moves to the newest end', mine, ['echo b', 'echo a', 'echo c']],
  ['up walks back and stops at the oldest', keys(mine, ['up', 'up', 'up', 'up']), ['echo c', 'echo a', 'echo b', 'echo b']],
  ['down walks forward to an empty line', keys(mine, ['up', 'up', 'down', 'down']), ['echo c', 'echo a', 'echo c', '']],
  ['down without recalling does nothing', keys(mine, ['down']), ['(nothing)']],
  ['nothing run yet: up does nothing', keys([], ['up']), ['(nothing)']],
];
for (const [label, got, want] of recall) {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  if (!ok) bad++;
  console.log((ok ? 'ok   ' : 'FAIL ') + label + (ok ? '' : ' -> ' + JSON.stringify(got)));
}
process.exit(bad ? 1 : 0);
