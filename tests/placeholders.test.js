// node tests/placeholders.test.js src/page/console.html
// The page's placeholder session (accepting an entry with parameters, typing, Tab, Esc), run
// against its real functions with a stand-in for the input line.
const fs = require('fs');
const html = fs.readFileSync(process.argv[2], 'utf8');
function grab(name) {
  const start = html.indexOf('  function ' + name + '(');
  if (start < 0) throw new Error('missing ' + name);
  const end = html.indexOf('\n  }\n', start);
  return html.slice(start, end + 4);
}
eval(html.match(/  var CALL_KINDS = .*;/)[0]);
var ph = null;
var input = {
  value: '', selectionStart: 0, selectionEnd: 0,
  setSelectionRange(a, b) { this.selectionStart = a; this.selectionEnd = b; },
};
function updateHint() {}
eval(['paramLabel', 'startPlaceholders', 'phSelect', 'phSync', 'phEscape'].map(grab).join('\n'));

// Typing: replace the selection with text, as the input line would.
function type(text) {
  const v = input.value;
  input.value = v.slice(0, input.selectionStart) + text + v.slice(input.selectionEnd);
  const c = input.selectionStart + text.length;
  input.setSelectionRange(c, c);
  phSync();
}
function selected() { return input.value.slice(input.selectionStart, input.selectionEnd); }

const effect = { v: 'Effect', k: 'member', form: { params: [{ name: 'index', type: 'int' }, { name: 'name', optional: true }], returns: 'actoreffect' } };
const relay = { v: 'relay', k: 'command', form: { params: [{ name: 'target' }, { literal: '-echo' }, { name: 'message', repeat: true }] } };

let bad = 0;
function check(label, got, want) {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  if (!ok) bad++;
  console.log((ok ? 'ok   ' : 'FAIL ') + label + (ok ? '' : '\n     got  ' + JSON.stringify(got) + '\n     want ' + JSON.stringify(want)));
}

// A member call: placeholders, first one selected.
input.value = 'echo ${Me.Ef'; input.setSelectionRange(12, 12);
startPlaceholders(10, effect, '');
check('member call inserted', input.value, 'echo ${Me.Effect[index,name]');
check('first placeholder selected', selected(), 'index');
type('3');
check('typing fills it', input.value, 'echo ${Me.Effect[3,name]');
phSelect(1);
check('Tab selects the next', selected(), 'name');
phEscape();
check('Esc drops the unfilled rest', input.value, 'echo ${Me.Effect[3]');
check('session over', ph, null);

// Nothing filled: Esc removes the brackets too.
input.value = 'echo ${Me.'; input.setSelectionRange(10, 10);
startPlaceholders(10, effect, '');
phEscape();
check('Esc with nothing filled', input.value, 'echo ${Me.Effect');

// Both filled: Esc keeps both.
input.value = ''; input.setSelectionRange(0, 0);
startPlaceholders(0, effect, '');
type('1'); phSelect(1); type('Grace');
phEscape();
check('Esc with all filled', input.value, 'Effect[1,Grace]');

// A command: spaces; the literal stays put and Tab skips it.
input.value = 're'; input.setSelectionRange(2, 2);
startPlaceholders(0, relay, '');
check('command inserted', input.value, 'relay target -echo message...');
type('all');
phSelect(1);
check('Tab skips the literal', selected(), 'message...');
phEscape();
check('Esc on a command (the literal goes with the rest)', input.value, 'relay all');

// Shift+Tab goes back; Tab past the last ends the session with the caret after the call.
input.value = ''; input.setSelectionRange(0, 0);
startPlaceholders(0, effect, ' rest');
phSelect(1); phSelect(-1);
check('Shift+Tab goes back', selected(), 'index');
phSelect(1); phSelect(1);
check('Tab past the last ends it', [ph, input.selectionStart], [null, 'Effect[index,name]'.length]);

// An edit outside the placeholders ends the session.
input.value = ''; input.setSelectionRange(0, 0);
startPlaceholders(0, effect, '');
input.setSelectionRange(0, 0); type('x');
check('typing elsewhere ends it', ph, null);

process.exit(bad ? 1 : 0);
