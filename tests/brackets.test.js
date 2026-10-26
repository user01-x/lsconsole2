// node tests/brackets.test.js src/page/console.html
// Pulls the bracket functions out of console.html and checks auto-closing and highlighting.
const fs = require('fs');
const html = fs.readFileSync(process.argv[2], 'utf8');
function grab(name) {
  const start = html.indexOf('  function ' + name + '(');
  if (start < 0) throw new Error('missing ' + name);
  const end = html.indexOf('\n  }\n', start);
  return html.slice(start, end + 4);
}
eval(html.match(/  var OPEN = .*;/)[0]);
eval(['bracketPairs', 'balanced', 'bracketEdit', 'bracketMarks'].map(grab).join('\n'));

let failed = 0;
function check(label, got, want) {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  if (!ok) failed++;
  console.log((ok ? 'ok   ' : 'FAIL ') + label + (ok ? '' : '  got ' + JSON.stringify(got) + ' want ' + JSON.stringify(want)));
}
// One typed character at the caret (replacing the selection [s, e)), as the input would do it.
function type(v, s, e, ch, ph) {
  const after = v.slice(0, s) + ch + v.slice(e);
  const r = bracketEdit({ v, s, e }, after, s + 1, !!ph);
  return r ? [r.v, r.s, r.e] : [after, s + 1, s + 1];
}
function backspace(v, s) {
  const after = v.slice(0, s - 1) + v.slice(s);
  const r = bracketEdit({ v, s, e: s }, after, s - 1, false);
  return r ? [r.v, r.s] : [after, s - 1];
}
// The marks as a string under the line: a = touching, i = around, x = unmatched.
function marks(v, s) {
  const m = bracketMarks(v, s, s);
  let r = '';
  for (let i = 0; i < v.length; i++) r += m[i] === 'at' ? 'a' : m[i] === 'in' ? 'i' : m[i] === 'bad' ? 'x' : ' ';
  return r.replace(/\s+$/, '');
}

check('${ closes', type('echo $', 6, 6, '{'), ['echo ${}', 7, 7]);
check('[ closes before }', type('echo ${Math.Calc}', 16, 16, '['), ['echo ${Math.Calc[]}', 17, 17]);
check('( closes at the end', type('echo ', 5, 5, '('), ['echo ()', 6, 6]);
check('no close before a word', type('echo abc', 5, 5, '['), ['echo [abc', 6, 6]);
check('wraps the selection', type('echo abc', 5, 8, '['), ['echo [abc]', 6, 9]);
check('placeholder selected: replaced, not wrapped', type('x[index]', 2, 7, '(', true), ['x[()]', 3, 3]);
check('] steps over', type('echo ${Math.Calc[]}', 17, 17, ']'), ['echo ${Math.Calc[]}', 18, 18]);
check('} steps over', type('echo ${Math.Calc[]}', 18, 18, '}'), ['echo ${Math.Calc[]}', 19, 19]);
check('unbalanced: ] is inserted', type('echo [[]', 7, 7, ']'), ['echo [[]]', 8, 8]);
check('backspace in an empty pair', backspace('echo ${}', 7), ['echo $', 6]);
check('backspace elsewhere', backspace('echo ab', 7), ['echo a', 6]);

check('caret inside []: [] only', marks('${Math.Calc[]}', 12), '           aa');
check('caret after ]: [] and {}', marks('${Math.Calc[]}', 13), ' i         aai');
check('caret in a name: the pair around', marks('${Math.Calc[]}', 5), ' i           i');
check('unmatched', marks('echo ${Me.Name', 3), '      x');
check('caret after the last }', marks('${Me}', 5), ' a  a');

if (failed) { console.log(failed + ' failed'); process.exit(1); }
console.log('all passed');
