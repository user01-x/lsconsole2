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
process.exit(bad ? 1 : 0);
