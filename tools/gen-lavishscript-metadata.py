"""Builds the built-in LavishScript/InnerSpace console metadata (src/metadata/lavishscript.json) from the
VSCode plugin's lavishscript_api.yml, in the console metadata schema (README.md).

    python tools/gen-lavishscript-metadata.py <path to lavishscript_api.yml>

The output is committed; this is rerun only when the YAML improves. What maps:
  datatypes -> types: description, inherits, members/methods; a name repeated in a type's list is
               one entry with several forms (overloads); a member's "type"/"returns" is the form's
               return type ("void" dropped)
  tlos      -> objects: one form from "parameters", returning "returns"
  commands  -> commands: forms parsed from the "syntax" strings (Markdown-ish:
               `Relay <[string](#..) `target`> "-echo" <... [string](#..) `message`)
  events    -> events: description (the YAML has no event parameters)
Static members and initializers are not carried over.
"""
import json
import re
import sys
from collections import OrderedDict

import yaml


def clean(s):
    return (s or '').strip()


def param(p):
    out = OrderedDict()
    out['name'] = clean(p.get('name')) or 'value'
    if clean(p.get('type')):
        out['type'] = clean(p.get('type'))
    if p.get('optional'):
        out['optional'] = True
    if p.get('defaultValue') not in (None, ''):
        out['description'] = 'default: %s' % p['defaultValue']
    return out


def entry_forms(items):
    """[member, member, ...] (a name may repeat: overloads) -> {name: entry}"""
    entries = OrderedDict()
    for m in items or []:
        name = clean(m.get('name'))
        if not name:
            continue
        form = OrderedDict()
        params = [param(p) for p in (m.get('parameters') or []) if isinstance(p, dict)]
        if params:
            form['params'] = params
        ret = clean(m.get('returns') or m.get('type'))
        if ret and ret.lower() != 'void':
            form['returns'] = ret
        e = entries.setdefault(name, OrderedDict())
        desc = clean(m.get('description'))
        if desc and 'description' not in e:
            e['description'] = desc
        elif desc and desc != e.get('description'):
            form['description'] = desc
        e.setdefault('forms', []).append(form)
    for e in entries.values():
        if e['forms'] == [{}]:
            del e['forms']  # one form, no parameters, nothing returned: the default
    return entries


# ---- command syntax -------------------------------------------------------------------------
LINK = re.compile(r'\[([^\[\]]*)\]\(#[^)]*\)')  # [string](#type-string) -> string; never spans a "[" group


def syntax_form(cmd, s):
    """`Bind` "-delete" <[string](#type-string) `name` -> {"params": [...]}"""
    s = LINK.sub(r'\1', s).replace('`', '')
    s = s.strip()
    if s.lower().startswith(cmd.lower()):
        s = s[len(cmd):]
    params, i = [], 0
    while i < len(s):
        ch = s[i]
        if ch.isspace():
            i += 1
            continue
        if ch in '<[':
            close = '>' if ch == '<' else ']'
            depth, j = 1, i + 1
            while j < len(s) and depth:
                if s[j] == ch:
                    depth += 1
                elif s[j] == close:
                    depth -= 1
                j += 1
            inner = s[i + 1:j - 1] if depth == 0 else s[i + 1:]  # an unclosed group runs to the end
            params.append(group(inner.strip(), optional=(ch == '[')))
            i = j
            continue
        if ch == '"':
            j = s.find('"', i + 1)
            j = len(s) if j < 0 else j
            params.append(OrderedDict([('literal', s[i + 1:j])]))
            i = j + 1
            continue
        # a bare word: treat as a literal token
        j = i
        while j < len(s) and not s[j].isspace() and s[j] not in '<["':
            j += 1
        params.append(OrderedDict([('literal', s[i:j])]))
        i = j
    params = merge_alternatives([p for p in params if p])
    return OrderedDict([('params', params)]) if params else OrderedDict()


def merge_alternatives(params):
    """"on"|"off" written outside a group parses as literal, literal "|", literal: one parameter
    with those values."""
    out, i = [], 0
    while i < len(params):
        p = params[i]
        if 'literal' in p and i + 2 < len(params) and params[i + 1].get('literal') == '|':
            values = [p['literal']]
            while i + 2 < len(params) and params[i + 1].get('literal') == '|' and 'literal' in params[i + 2]:
                values.append(params[i + 2]['literal'])
                i += 2
            out.append(OrderedDict([('name', 'option'), ('values', values)]))
        else:
            out.append(p)
        i += 1
    return out


def group(inner, optional):
    p = OrderedDict()
    repeat = inner.startswith('...')
    if repeat:
        inner = inner[3:].strip()
    if '"' in inner:  # "-press"|"-release"  or  "-host"
        values = re.findall(r'"([^"]*)"', inner)
        p['name'] = values[0] if len(values) == 1 else 'option'
        p['values'] = values
    else:
        words = inner.split()
        if len(words) >= 2:
            p['type'], p['name'] = words[0], words[-1]
        elif words:
            p['name'] = words[0]
        else:
            return None
    if optional:
        p['optional'] = True
    if repeat:
        p['repeat'] = True
    return p


# Hand-written where the YAML's syntax is garbled or a parameter should complete from a console list.
OVERRIDES = {
    'DeclareVariable': {
        'description': 'Declares a variable.',
        'forms': [{'params': [
            {'name': 'name', 'type': 'string'},
            {'name': 'type', 'source': 'types'},
            {'name': 'scope', 'values': ['local', 'script', 'global', 'globalkeep', 'object'], 'optional': True},
            {'name': 'value', 'optional': True, 'repeat': True},
        ]}],
    },
}
# Parameters of these commands complete from a console list: command -> {param name: source}
SOURCES = {
    'RunScript': {'filename': 'files'},
    'EndScript': {'filename': 'scripts'},
}


def main(src):
    d = yaml.safe_load(open(src, encoding='utf-8'))
    out = OrderedDict([('schema', 1), ('name', 'LavishScript'), ('version', d.get('extraction_date', '')),
                       ('types', OrderedDict()), ('objects', OrderedDict()), ('commands', OrderedDict()),
                       ('events', OrderedDict())])
    for name, t in sorted((d.get('datatypes') or {}).items()):
        e = OrderedDict()
        if clean(t.get('description')):
            e['description'] = clean(t['description'])
        if clean(t.get('inherits')):
            e['inherits'] = clean(t['inherits'])
        members = entry_forms(t.get('members'))
        methods = entry_forms(t.get('methods'))
        if members:
            e['members'] = members
        if methods:
            e['methods'] = methods
        out['types'][name.lower()] = e
    for name, t in sorted((d.get('tlos') or {}).items()):
        e = OrderedDict()
        if clean(t.get('description')):
            e['description'] = clean(t['description'])
        form = OrderedDict()
        params = [param(p) for p in (t.get('parameters') or []) if isinstance(p, dict)]
        if params:
            form['params'] = params
        if clean(t.get('returns')):
            form['returns'] = clean(t['returns'])
        if form:
            e['forms'] = [form]
        out['objects'][name] = e
    for name, c in sorted((d.get('commands') or {}).items()):
        e = OrderedDict()
        if clean(c.get('description')):
            e['description'] = clean(c['description'])
        forms = [f for f in (syntax_form(name, s) for s in (c.get('syntax') or []))]
        forms = [f for f in forms if f] + ([OrderedDict()] if any(not f for f in forms) else [])
        if forms and forms != [OrderedDict()]:
            e['forms'] = forms
        for f in e.get('forms', []):
            for p in f.get('params', []):
                if p.get('name') in SOURCES.get(name, {}):
                    p['source'] = SOURCES[name][p['name']]
        if name in OVERRIDES:
            e = OrderedDict(OVERRIDES[name])
        out['commands'][name] = e
    for name, ev in sorted((d.get('events') or {}).items()):
        e = OrderedDict()
        if clean(ev.get('description')):
            e['description'] = clean(ev['description'])
        out['events'][name] = e
    dst = 'src/metadata/lavishscript.json'
    with open(dst, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(out, f, indent=1, ensure_ascii=False)
        f.write('\n')
    forms = sum(len(m.get('forms', [{}])) for t in out['types'].values() for k in ('members', 'methods') for m in t.get(k, {}).values())
    print('%s: %d types (%d member/method forms), %d objects, %d commands, %d events' % (
        dst, len(out['types']), forms, len(out['objects']), len(out['commands']), len(out['events'])))


if __name__ == '__main__':
    main(sys.argv[1])
