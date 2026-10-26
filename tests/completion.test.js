// node tests/completion.test.js src/page/console.html
// Pulls the completion functions out of console.html and checks the context rules.
const fs = require('fs');
const html = fs.readFileSync(process.argv[2], 'utf8');
function grab(name) {
  const start = html.indexOf('  function ' + name + '(');
  if (start < 0) throw new Error('missing ' + name);
  const end = html.indexOf('\n  }\n', start);
  return html.slice(start, end + 4);
}
var data = {
  types: ['int', 'actor', 'string', 'unistring', 'mutablestring'], tlos: ['Actor', 'Me', 'Mercenary', 'EQ2', 'Script', 'LavishScript'], events: ['Zerada_FrontalAssault', 'Zom_GreenReport', 'OgreBotDone'],
  commands: ['echo', 'End', 'EndScript', 'Run', 'RunScript', 'relay', 'lsconsole2', 'ext'],
  aliases: [{ n: 'actors', w: 'runscript custom/ActorLoop' }, { n: 'edit', w: 'runscript custom/Editor' }],
  extensions: [{ n: 'ISXEQ2', l: true }, { n: 'ISXEQ2_test', l: false }, { n: 'ISXOgreX', l: true }, { n: 'LSConsole2', l: true }],
  scripts: ['OgreBot', 'statuspoller', 'Buffer:OgreBot'], variables: ['MyVar', 'Meh', { n: 'lavish', v: '42' }], files: ['custom/ActorLoop', 'custom/AchievementCheck', 'Ogre/Start'],
};
var sent = [], send = function (m) { sent.push(m); };
var typeInfo = {
  Me: { type: 'character', members: ['Name', 'Level', 'Health'], methods: ['Face', 'DoTarget'], at: Date.now() },
  'OgreBotAPI.Bot': { type: 'ogrebot', members: ['Paused'], methods: ['Pause', 'Resume'], at: Date.now() },
}, typeAsked = {};
eval(html.match(/  var CHAIN_SEP = .*;/)[0]); // the page's own patterns and tables
eval(html.match(/  var CHAIN_END = .*;/)[0]);
eval(html.match(/  var SOURCES = .*;/)[0]);
eval(html.match(/  var CALL_KINDS = .*;/)[0]);
var ph = null;
var meta = { commands: {}, objects: {}, events: {} };
eval(['beginningAt', 'historyQuery', 'words', 'matchesAll', 'named', 'variables', 'has', 'byLower', 'extend', 'withForms',
  'typeOf', 'typeByName', 'splitChain', 'argCount', 'formReturn', 'resolveChain', 'chainInfo', 'memberContext', 'callAt',
  'finishCall', 'paramEntries', 'contextAt', 'filterNames', 'spellings', 'entryNamed', 'subsOf', 'subRows'].map(grab).join('\n'));
var commandSpellings = null;
eval(html.match(/  var SCOPE_RE = .*;/)[0]);
eval(grab('scopeVarsOf'));
scopeVars['Script[foo].VariableScope'] = { names: ['Mine', 'Count'], at: Date.now() };

// Metadata, as the page holds it (byLower keeps the written name as _n).
meta.commands = byLower({ lsconsole2: { forms: [{ params: [{ name: 'what', values: ['toggle', 'show', 'hide', 'status'] }] }] },
  Extension: { abbreviate: 3, forms: [{ params: [{ name: 'name', values: ['ISXEQ2', 'ISXOgreX'] }] }],
    subcommands: { '-list': { description: 'lists' }, '-unload': { forms: [{ params: [{ name: 'loaded', values: ['ISXEQ2'] }] }] } } },
  oc: { subcommands: { '!c': { also: ['!ci'], subcommands: {
    '-Jump': { forms: [{ params: [{ name: 'forwho', values: ['all', 'igw:Me'] }] }] },
    '-Pause': { forms: [{ params: [{ name: 'forwho' }] }] } } } } } });
meta.objects = byLower({ Actor: { forms: [{ params: [{ name: 'id', type: 'int' }], returns: 'actor' }] } });
var charDocs = { members: byLower({ Name: { forms: [{ returns: 'string' }] },
  Effect: { forms: [{ params: [{ name: 'index', type: 'int' }], returns: 'actoreffect' }, { params: [{ name: 'name' }], returns: 'actoreffect' }] },
  Ghost: { description: 'removed long ago' } }), methods: byLower({}) };
typeInfo.Me.live = true; typeInfo.Me.docs = charDocs;
typeInfo['OgreBotAPI.Bot'].live = true; typeInfo['OgreBotAPI.Bot'].docs = { members: {}, methods: {} };
typeInfo['@character'] = { type: 'character', live: true, members: ['Name', 'Level', 'Health', 'Effect'], methods: [], docs: charDocs, at: Date.now() };
typeInfo['@actoreffect'] = { type: 'actoreffect', live: true, members: ['Duration', 'Name'], methods: ['Cancel'], docs: { members: {}, methods: {} }, at: Date.now() };
typeInfo['@actor'] = { type: 'actor', live: true, members: ['Distance', 'Name'], methods: ['DoTarget'], docs: { members: {}, methods: {} }, at: Date.now() };

function show(text) {
  const c = contextAt(text);
  if (!c) return null;
  if (c.history) return { history: c.history, from: c.from };
  return { title: c.title, token: c.token, from: c.from, names: filterNames(c).map(e => e.v) };
}
const cases = [
  ['', null],
  ['ec', { title: 'commands, aliases, variables and objects', token: 'ec', from: 0, names: ['echo'] }],
  ['e', { title: 'commands, aliases, variables and objects', token: 'e', from: 0, names: ['ext', 'echo', 'edit', 'End', 'EQ2', 'EndScript', 'Me', 'Meh', 'relay', 'Mercenary', 'lsconsole2'] }],
  ['declare x ', { title: 'types', token: '', from: 10, names: ['int', 'actor', 'string', 'unistring', 'mutablestring'] }],
  ['declarevariable foo in', { title: 'types', token: 'in', from: 20, names: ['int', 'string', 'unistring', 'mutablestring'] }],
  ['echo declare x i', null],
  ['lstype st', { title: 'types', token: 'st', from: 7, names: ['string', 'unistring', 'mutablestring'] }],
  ['LSType ', { title: 'types', token: '', from: 7, names: ['int', 'actor', 'string', 'unistring', 'mutablestring'] }],
  ['echo lstype st', null],
  ['echo Event[ze', { title: 'events', token: 'ze', from: 11, names: ['Zerada_FrontalAssault'] }],
  ['Me.', { title: 'members of Me (character)', token: '', from: 3, names: ['Name', 'Level', 'Health', 'Ghost', 'Effect', 'Effect'] }], // Ghost, Effect: documented, not live
  ['Me:fa', { title: 'methods of Me (character)', token: 'fa', from: 3, names: ['Face'] }],
  ['echo ${Me.le', { title: 'members of Me (character)', token: 'le', from: 10, names: ['Level'] }],
  ['relay all Me:', { title: 'methods of Me (character)', token: '', from: 13, names: ['Face', 'DoTarget'] }],
  ['Target.', { title: 'members of Target', token: '', from: 7, names: [] }],
  // metadata: a parameter's values; a chain through a documented return type; an object's return
  ['lsconsole2 s', { title: 'what of lsconsole2', token: 's', from: 11, names: ['show', 'status'] }],
  ['echo ${Me.Effect[1].', { title: 'members of Me.Effect[1] (actoreffect)', token: '', from: 20, names: ['Name', 'Duration'] }],
  ['echo ${Actor[5]:', { title: 'methods of Actor[5] (actor)', token: '', from: 16, names: ['DoTarget'] }],
  ['OgreBotAPI.Bot:', { title: 'methods of OgreBotAPI.Bot (ogrebot)', token: '', from: 15, names: ['Pause', 'Resume'] }],
  ['echo ${OgreBotAPI.Bot.pa', { title: 'members of OgreBotAPI.Bot (ogrebot)', token: 'pa', from: 22, names: ['Paused'] }],
  ['echo ${Me.Group[1].Na', { title: 'members of Me.Group[1]', token: 'Na', from: 19, names: [] }],
  ['relay all Actor[npc,radius,10]:Do', { title: 'methods of Actor[npc,radius,10] (actor)', token: 'Do', from: 31, names: ['DoTarget'] }], // the documented return
  ['echo ${Me.Group[1', null],
  ['echo ', null],
  ['hist foo', { history: ['foo'], from: 0 }],
  ['relay all ec', { title: 'commands, aliases, variables and objects', token: 'ec', from: 10, names: ['echo'] }],
  ['relay "is1 is2" hist x', { history: ['x'], from: 16 }],
  ['relay all', null],
  ['end ', { title: 'running scripts', token: '', from: 4, names: ['OgreBot', 'statuspoller', 'Buffer:OgreBot'] }],
  ['endscript og', { title: 'running scripts', token: 'og', from: 10, names: ['OgreBot', 'Buffer:OgreBot'] }],
  ['echo end x', null],
  ['relay all end st', { title: 'running scripts', token: 'st', from: 14, names: ['statuspoller'] }],
  ['run cust', { title: 'scripts', token: 'cust', from: 4, names: ['custom/ActorLoop', 'custom/AchievementCheck'] }],
  ['echo ${M', { title: 'objects and variables', token: 'M', from: 7, names: ['Me', 'Meh', 'MyVar', 'Mercenary'] }],
  ['echo ${La', { title: 'objects and variables', token: 'La', from: 7, names: ['LavishScript', 'lavish'] }], // the user's example
  ['echo ${Me.N', { title: 'members of Me (character)', token: 'N', from: 10, names: ['Name'] }],
  ['echo ${Me} ${', { title: 'objects and variables', token: '', from: 13, names: [] }],
  ['echo ${Script[og', { title: 'running scripts', token: 'og', from: 14, names: ['OgreBot', 'Buffer:OgreBot'] }],
  ['echo ${Script[OgreBot]', null],
  ['ext isx', { title: 'extensions', token: 'isx', from: 4, names: ['ISXEQ2', 'ISXOgreX', 'ISXEQ2_test'] }],
  ['Extension ', { title: 'extensions', token: '', from: 10, names: ['ISXEQ2', 'ISXOgreX', 'LSConsole2', 'ISXEQ2_test'] }],
  ['ext -unload ', { title: 'loaded extensions', token: '', from: 12, names: ['ISXEQ2', 'ISXOgreX', 'LSConsole2'] }],
  ['relay all exten -unload ls', { title: 'loaded extensions', token: 'ls', from: 24, names: ['LSConsole2'] }],
  ['echo ext isx', null],
  // Sub-commands (oc !c -Jump <forwho>): each level lists the next.
  ['oc ', { title: 'subcommands of oc', token: '', from: 3, names: ['!c', '!ci'] }],
  ['oc !CI -', { title: 'subcommands of oc !ci', token: '-', from: 7, names: ['-Jump', '-Pause'] }],
  ['oc !c -Jump ', { title: 'forwho of oc !c -Jump', token: '', from: 12, names: ['all', 'igw:Me'] }],
  ['oc !c -Ju ', null], // -Jump can't be abbreviated
  ['echo ${Script[foo].VariableScope.Mi', { title: 'members of Script[foo].VariableScope', token: 'Mi', from: 33, names: ['Mine'] }],
  ['echo ${Script[foo].VariableScope:Mi', { title: 'methods of Script[foo].VariableScope', token: 'Mi', from: 33, names: [] }], // no variables for methods
];
let bad = 0;
for (const [text, want] of cases) {
  const got = show(text);
  const ok = JSON.stringify(got) === JSON.stringify(want);
  if (!ok) bad++;
  console.log((ok ? 'ok   ' : 'FAIL ') + JSON.stringify(text) + (ok ? '' : '\n     got  ' + JSON.stringify(got) + '\n     want ' + JSON.stringify(want)));
}
console.log('asked for types: ' + JSON.stringify(sent));
// The call being typed: entry, argument index, chosen form.
function call(text) {
  const c = callAt(text);
  return c && c.doc ? { name: c.name, index: c.index, param: c.param ? c.param.name : null, token: c.token } : null;
}
const calls = [
  ['echo ${Me.Effect[', { name: 'Effect', index: 0, param: 'index', token: '' }],
  ['echo ${Me.Effect[abc', { name: 'Effect', index: 0, param: 'index', token: 'abc' }],
  ['echo ${Actor[1', { name: 'Actor', index: 0, param: 'id', token: '1' }],
  ['echo ${Actor[1,', { name: 'Actor', index: 1, param: null, token: '' }],
  ['lsconsole2 ', { name: 'lsconsole2', index: 0, param: 'what', token: '' }],
  ['relay all lsconsole2 st', { name: 'lsconsole2', index: 0, param: 'what', token: 'st' }],
  ['echo hi', null],
  ['exte -unload ', { name: 'Extension -unload', index: 0, param: 'loaded', token: '' }], // abbreviate: 3
  ['ex -unload ', null], // shorter than 3
  ['extension isx', { name: 'Extension', index: 0, param: 'name', token: 'isx' }],
  ['oc !ci -Jump a', { name: 'oc !ci -Jump', index: 0, param: 'forwho', token: 'a' }],
  ['oc !c -jump ', { name: 'oc !c -Jump', index: 0, param: 'forwho', token: '' }],
  ['oc !c -Ju ', { name: 'oc !c', index: 1, param: null, token: '' }],
];
for (const [text, want] of calls) {
  const got = call(text);
  const ok = JSON.stringify(got) === JSON.stringify(want);
  if (!ok) bad++;
  console.log((ok ? 'ok   ' : 'FAIL ') + 'call ' + JSON.stringify(text) + (ok ? '' : '\n     got  ' + JSON.stringify(got) + '\n     want ' + JSON.stringify(want)));
}
process.exit(bad ? 1 : 0);
