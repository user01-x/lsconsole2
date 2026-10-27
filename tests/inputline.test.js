// node tests/inputline.test.js src/page/console.html
// Pulls the input line's colouring and the history suggestion out of console.html and checks them.
const fs = require('fs');
const html = fs.readFileSync(process.argv[2], 'utf8');
function grab(name) {
  const start = html.indexOf('  function ' + name + '(');
  if (start < 0) throw new Error('missing ' + name);
  const end = html.indexOf('\n  }\n', start);
  return html.slice(start, end + 4);
}
eval(html.match(/  var OPEN = .*;/)[0]);
var data = { commands: ['echo', 'Extension', 'relay', 'RunScript'], aliases: [{ n: 'eq', w: 'echo' }],
             tlos: ['Me', 'Math', 'Time'], types: ['int', 'string'], variables: ['dtNow'] };
// What the console knows about each chain (as the extension would have answered).
var types = {
  Me: { live: true, members: ['Name', 'Group', 'Level'], methods: ['DoTarget'] },
  'Me.Group[1]': { live: true, members: ['Name', 'Distance'], methods: [] },
  Math: { live: true, members: ['Calc'], methods: [] },
};
function chainInfo(c) { return types[c] || null; }
eval(['variables', 'named', 'beginningAt', 'historyQuery', 'bracketPairs', 'knownCommand', 'knownRoot', 'hasName',
      'lineClasses', 'suggestion'].map(grab).join('\n'));

let failed = 0;
function check(label, got, want) {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  if (!ok) failed++;
  console.log((ok ? 'ok   ' : 'FAIL ') + label + (ok ? '' : '\n     got  ' + JSON.stringify(got) + '\n     want ' + JSON.stringify(want)));
}
// The coloured words, as "word:class" in order (caret at the end unless given).
function words(v, caret) {
  const cls = lineClasses(v, caret === undefined ? v.length : caret), out = [];
  for (let i = 0; i < v.length; ) {
    let j = i + 1;
    while (j < v.length && cls[j] === cls[i]) j++;
    if (cls[i]) out.push(v.slice(i, j) + ':' + cls[i].replace('tk-', ''));
    i = j;
  }
  return out;
}

check('a command', words('echo hi', 0), ['echo:cmd']);
check('an abbreviation counts', words('ext ISXEQ2'), ['ext:cmd']);
check('an alias', words('eq hi'), ['eq:cmd']);
check('an unknown command is red', words('ecoh hi'), ['ecoh:bad']);
check('...but not while it is typed', words('ecoh'), ['ecoh:cmd']);
check('hist is ours', words('hist relay'), ['hist:cmd']);
check('relay and its target', words('relay all echo hi'), ['relay:cmd', 'all:target', 'echo:cmd']);
check('${ } and the object', words('echo ${Me.Name}'), ['echo:cmd', '${:ds', 'Me:obj', '}:ds']);
check('an unknown member is red', words('echo ${Me.Nmae}'), ['echo:cmd', '${:ds', 'Me:obj', 'Nmae:bad', '}:ds']);
check('...but not at the caret', words('echo ${Me.Nma', 13), ['echo:cmd', '${:ds', 'Me:obj']);
check('members through [args]', words('echo ${Me.Group[1].Dist}'), ['echo:cmd', '${:ds', 'Me:obj', 'Dist:bad', '}:ds']);
check('an unknown object is red', words('echo ${Mee.Name}'), ['echo:cmd', '${:ds', 'Mee:bad', '}:ds']);
check('a variable', words('echo ${dtNow}'), ['echo:cmd', '${:ds', 'dtNow:obj', '}:ds']);
check('unknown type: nothing past it is judged', words('echo ${Time.Whatever}'), ['echo:cmd', '${:ds', 'Time:obj', '}:ds']);
check('a string, with a data sequence inside', words('echo "hi ${Me.Name} x"'),
      ['echo:cmd', '"hi :str', '${:ds', 'Me:obj', '}:ds', ' x":str']);
check('a chain at the beginning', words('Me:DoTraget', 0), ['Me:obj', 'DoTraget:bad']);
check('nested ${} in arguments', words('echo ${Math.Calc[${dtNow}+1]}'),
      ['echo:cmd', '${:ds', 'Math:obj', '${:ds', 'dtNow:obj', '}:ds', '}:ds']);
data.commands = [];
check('no red before the lists arrive', words('ecoh hi'), ['ecoh:cmd']);

var sessionHist = ['echo mine', 'run ogre'], history = ['echo theirs', 'echo mine', 'run ogre fast'];
check('this session first', suggestion('run o'), 'gre');
check('then everyone', suggestion('echo t'), 'heirs');
check('newest of this session', suggestion('echo '), 'mine');
check('nothing to suggest', suggestion('zzz'), '');
check('not for an empty line', suggestion(''), '');
check('not for a hist query', suggestion('hist e'), '');

if (failed) { console.log(failed + ' failed'); process.exit(1); }
console.log('all passed');
