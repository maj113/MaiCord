"""Generates the SDK conformance unit for MaiCord's SDK-free headers.

The one unit includes the SDK headers listed in the headers' MAICORD_SDK_INCLUDES
blocks, asserts that the SDK declares every function, variable, typedef, tag and
macro the headers declare, includes the headers (whose redeclarations the
compiler then checks) and compares each type they define with the SDK's:
record sizes, alignments, field offsets and types, enumerator values, interface
IIDs, bases, vtable slots and method signatures, and the values of their
constants and GUIDs.

The parser accepts the restricted C++ these headers are written in and fails
on anything else, so a construct it cannot check is never skipped silently.
"""

import argparse
import pathlib
import re
import sys

TOKEN = re.compile(
    r'\s+|(?P<string>L?"(?:\\.|[^"\\])*")|(?P<char>L?\'(?:\\.|[^\'\\])*\')'
    r'|(?P<number>\.?\d[\w.]*(?:[eEpP][+-]\w*)?)|(?P<ident>[A-Za-z_]\w*)|(?P<punct>::|->|\.\.\.|[^\s\w])')

BLOCK_BEGIN = 'MAICORD_SYS_TYPES_BEGIN'
BLOCK_END = 'MAICORD_SYS_TYPES_END'
SDK_GUARD = re.compile(r'^#\s*if\s+defined\s*\(\s*MAICORD_SDK_INCLUDES\s*\)\s*$')
CALLING_CONVENTIONS = {'__stdcall', '__cdecl', '__fastcall', '__vectorcall', 'WINAPI', 'STDMETHODCALLTYPE',
                       'CALLBACK', 'APIENTRY', 'WSAAPI', 'NTAPI'}
TYPE_KEYWORDS = {'struct', 'union', 'class', 'enum'}


class ParseError(Exception):
    pass


def strip_comments(text):
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            if j < 0:
                raise ParseError('unterminated comment')
            out.append('\n' * text.count('\n', i, j))
            i = j + 2
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def tokenize(text):
    tokens = []
    for match in TOKEN.finditer(text):
        if match.lastgroup:
            tokens.append(match.group(match.lastgroup))
    return tokens


class Header:
    def __init__(self, path):
        self.path = path
        self.sdk_includes = []
        self.macros = []
        self.functions = []
        self.typedefs = []
        self.tags = []
        self.records = []
        self.enums = []
        self.block_typedefs = []
        self.coclasses = []
        self.guids = []
        self.constants = []
        self.parse(path.read_text(encoding='utf-8'))

    def parse(self, text):
        # Diagnostic text is prose (apostrophes) and declares nothing.
        text = re.sub(r'(?m)^\s*#\s*(?:error|warning)\b.*$', '', text)
        text = strip_comments(text).replace('\\\n', ' ')
        code = []
        lines = text.split('\n')
        index = 0
        while index < len(lines):
            line = lines[index].strip()
            index += 1
            if not line.startswith('#'):
                code.append(line)
                continue
            if SDK_GUARD.match(line):
                depth = 1
                while index < len(lines):
                    inner = lines[index].strip()
                    index += 1
                    if re.match(r'#\s*if', inner):
                        depth += 1
                    elif re.match(r'#\s*endif', inner):
                        depth -= 1
                        if not depth:
                            break
                    elif depth == 1 and re.match(r'#\s*(include\s*<|define\s)', inner):
                        self.sdk_includes.append(inner)
                    elif inner:
                        raise ParseError(f'unexpected line in the SDK block: {inner}')
                continue
            match = re.match(r'#\s*define\s+(\w+)', line)
            if match and not match.group(1).startswith('MAICORD_'):
                self.macros.append(match.group(1))
        self.parse_scope(tokenize('\n'.join(code)), 0, False)

    def parse_scope(self, tokens, i, in_block, closing=False):
        while i < len(tokens):
            token = tokens[i]
            if token == '}' and closing:
                return i + 1
            if token == BLOCK_BEGIN:
                if in_block:
                    raise ParseError('nested types block')
                in_block = True
                i += 1
                continue
            if token == BLOCK_END:
                if not in_block:
                    raise ParseError('unmatched types block end')
                in_block = False
                i += 1
                continue
            if token == 'namespace':
                # MaiCord's own namespaces hold nothing the SDK declares.
                i = skip_to_matching(tokens, tokens.index('{', i)) + 1
                continue
            if token == 'extern' and tokens[i + 1] in ('"C"', '"C++"') and tokens[i + 2] == '{':
                i = self.parse_scope(tokens, i + 3, in_block, True)
                continue
            end, statement = read_statement(tokens, i)
            i = end
            if statement:
                if in_block:
                    self.block_statement(statement)
                else:
                    self.global_statement(statement)
        if in_block:
            raise ParseError('unterminated types block')
        return i

    def global_statement(self, s):
        s = drop_attributes(drop_extern_c(s))
        if s[0] == 'DEFINE_GUID':
            self.guids.append(s[2])
            return
        if is_definition_only(s) or s[0] in ('static_assert', 'using'):
            return
        if s[0] == 'typedef':
            self.typedefs.extend(typedef_names(s))
            return
        if s[0] in TYPE_KEYWORDS:
            if '{' in s:
                raise ParseError(f'type defined outside a types block: {" ".join(s[:6])}')
            self.tags.append(declared_tag(s))
            return
        if s[0] == '#':
            return
        name = declarator_name(s)
        if name:
            self.functions.append(name)

    def block_statement(self, s):
        s = drop_attributes(drop_extern_c(s))
        head = s[0]
        if is_definition_only(s) or head == 'static_assert':
            return
        if '=' in s and '(' not in s[:s.index('=')] and head in ('const', 'static', 'extern'):
            # A constant the header defines: compared by value.
            self.constants.append(declarator_name(s[:s.index('=')]))
            return
        if head == 'typedef' and '{' in s:
            if 'enum' in s[:s.index('{')]:
                self.enums.append(parse_enum(s))
            else:
                self.records.append(parse_record(s))
            return
        if head == 'typedef':
            self.block_typedefs.extend(typedef_names(s))
            return
        if head == 'enum':
            self.enums.append(parse_enum(s))
            return
        if head in TYPE_KEYWORDS:
            if '{' not in s:
                uuid = declspec_uuid(s)
                if uuid:
                    self.coclasses.append(declared_tag(s))
                else:
                    self.tags.append(declared_tag(s))
                return
            self.records.append(parse_record(s))
            return
        raise ParseError(f'unsupported declaration in a types block: {" ".join(s[:8])}')


def first_call_paren(tokens):
    """The first '(' outside __declspec(...) and __pragma(...) groups."""
    i = 0
    while i < len(tokens):
        if tokens[i] in ('__declspec', '__pragma') and i + 1 < len(tokens) and tokens[i + 1] == '(':
            i = skip_to_matching(tokens, i + 1) + 1
            continue
        if tokens[i] == '(':
            return i
        i += 1
    raise ParseError(f'no parameter list: {" ".join(tokens)}')


def skip_to_matching(tokens, i):
    pairs = {'{': '}', '(': ')', '[': ']'}
    opener = tokens[i]
    closer = pairs[opener]
    depth = 0
    while i < len(tokens):
        if tokens[i] == opener:
            depth += 1
        elif tokens[i] == closer:
            depth -= 1
            if not depth:
                return i
        i += 1
    raise ParseError(f'unbalanced {opener}')


def read_statement(tokens, i):
    """Returns (next index, tokens) for one declaration ending in ';' or a body."""
    start = i
    while i < len(tokens):
        token = tokens[i]
        if token == ';':
            return i + 1, tokens[start:i]
        if token in ('(', '['):
            i = skip_to_matching(tokens, i) + 1
            continue
        if token == '{':
            close = skip_to_matching(tokens, i)
            statement = tokens[start:close + 1]
            if statement[0] in TYPE_KEYWORDS or statement[0] == 'typedef':
                # A type definition continues to its ';'.
                i = close + 1
                continue
            # A function body ends the statement.
            return close + 1, statement
        if token in (BLOCK_BEGIN, BLOCK_END, '}'):
            if i == start:
                return i + 1 if token == '}' else i, []
            raise ParseError(f'missing ; before {token}: {" ".join(tokens[start:i])}')
        i += 1
    raise ParseError('unterminated declaration')


INLINE = {'inline', '__inline', '__forceinline', 'constexpr', 'template'}


def is_definition_only(s):
    """Inline functions and templates: definitions with nothing the SDK declares globally."""
    head = s[:s.index('(')] if '(' in s else s
    return any(t in INLINE for t in head)


def drop_attributes(s):
    while len(s) > 1 and s[0] == '[' and s[1] == '[':
        s = s[s.index(']') + 2:]
    return s


def drop_extern_c(s):
    if len(s) > 1 and s[0] == 'extern' and s[1] == '"C"':
        return s[2:]
    return s


def without_declspecs(s):
    out = []
    i = 0
    while i < len(s):
        if s[i] in ('__declspec', 'alignas', '__pragma') and i + 1 < len(s) and s[i + 1] == '(':
            i = skip_to_matching(s, i + 1) + 1
            continue
        out.append(s[i])
        i += 1
    return out


def declspec_uuid(s):
    for i, token in enumerate(s):
        if token == 'uuid' and s[i + 1] == '(':
            return s[i + 2]
    return None


def declared_tag(s):
    s = without_declspecs(s)
    for token in s[1:]:
        if re.match(r'^[A-Za-z_]\w*$', token) and token not in ('final',):
            return token
    raise ParseError(f'tag without a name: {" ".join(s)}')


def typedef_names(s):
    s = without_declspecs(s[1:])
    # A function or function pointer typedef: the name is in the first group.
    if '(' in s and '{' not in s:
        open_index = s.index('(')
        group = s[open_index + 1:skip_to_matching(s, open_index)]
        identifiers = [t for t in group if re.match(r'^[A-Za-z_]\w*$', t) and t not in CALLING_CONVENTIONS]
        # TYPE (CALLCONV* NAME)(...) or TYPE (CALLCONV NAME)(...)
        if identifiers and group and (group[0] == '*' or group[0] in CALLING_CONVENTIONS):
            return [identifiers[-1]]
        # TYPE CALLCONV NAME(...)
        if re.match(r'^[A-Za-z_]\w*$', s[open_index - 1]) and s[open_index - 1] not in CALLING_CONVENTIONS:
            return [s[open_index - 1]]
        if identifiers and len(group) <= 3:
            return [identifiers[-1]]
    if '{' in s:
        s = s[skip_to_matching(s, s.index('{')) + 1:]
    names = []
    for declarator in split_top(s, ','):
        idents = [t for t in strip_arrays(declarator) if re.match(r'^[A-Za-z_]\w*$', t)]
        if not idents:
            raise ParseError(f'typedef without a name: {" ".join(s)}')
        names.append(idents[-1])
    return names


def strip_arrays(tokens):
    out = []
    i = 0
    while i < len(tokens):
        if tokens[i] == '[':
            i = skip_to_matching(tokens, i) + 1
            continue
        out.append(tokens[i])
        i += 1
    return out


def split_top(tokens, separator):
    parts = [[]]
    depth = 0
    for token in tokens:
        if token in '([{<':
            depth += 1
        elif token in ')]}>':
            depth -= 1
        if token == separator and depth == 0:
            parts.append([])
        else:
            parts[-1].append(token)
    return [part for part in parts if part]


def declarator_name(s):
    s = without_declspecs(s)
    for i, token in enumerate(s):
        if token == '(':
            if i and re.match(r'^[A-Za-z_]\w*$', s[i - 1]):
                return s[i - 1]
            return None
    # A variable declaration.
    idents = [t for t in strip_arrays(s) if re.match(r'^[A-Za-z_]\w*$', t)]
    return idents[-1] if idents else None


def parse_enum(s):
    s = without_declspecs(s)
    if s[0] == 'typedef':
        # typedef enum { ... } NAME: an unnamed enum named by its typedef.
        return {'name': s[-1], 'scoped': False, 'enumerators': enumerators_of(s)}
    scoped = s[1] in ('class', 'struct')
    name = s[2] if scoped else s[1]
    return {'name': name, 'scoped': scoped, 'enumerators': enumerators_of(s)}


def enumerators_of(s):
    body = s[s.index('{') + 1:skip_to_matching(s, s.index('{'))]
    return [part[0] for part in split_top(body, ',')]


def parse_record(s):
    uuid = declspec_uuid(s)
    s = without_declspecs(s)
    typedef_name = None
    if s[0] == 'typedef':
        # typedef struct { ... } NAME: an unnamed struct named by its typedef.
        typedef_name = s[-1]
        s = s[1:-1]
    kind = s[0]
    open_index = s.index('{')
    head = s[1:open_index]
    if not head and not typedef_name:
        raise ParseError('unnamed record')
    name = head[0] if head else typedef_name
    base = None
    if ':' in head:
        base_tokens = [t for t in head[head.index(':') + 1:] if t not in ('public', 'virtual')]
        if len(base_tokens) != 1:
            raise ParseError(f'{name}: one public base only')
        base = base_tokens[0]
    close = skip_to_matching(s, open_index)
    trailing = s[close + 1:]
    if trailing:
        raise ParseError(f'{name}: declarators after a record definition')
    fields = []
    methods = []
    parse_members(s[open_index + 1:close], fields, methods, name)
    return {'name': name, 'kind': kind, 'uuid': uuid, 'base': base, 'fields': fields, 'methods': methods}


def parse_members(body, fields, methods, owner):
    i = 0
    while i < len(body):
        if body[i] in ('public', 'private', 'protected') and body[i + 1] == ':':
            i += 2
            continue
        if body[i] == 'using':
            # using Base::name brings the base's overloads in.
            end = body.index(';', i)
            methods.append({'name': body[end - 1], 'unused': False, 'helper': True})
            i = end + 1
            continue
        if body[i] in ('union', 'struct') and body[i + 1] != '{' and body[i + 2] == '{':
            # A named nested type: its members are not the record's.
            close = skip_to_matching(body, i + 2)
            end = body.index(';', close)
            for field in [t for t in body[close + 1:end] if re.match(r'^[A-Za-z_]\w*$', t)]:
                fields.append({'name': field, 'typed': True, 'bits': False})
            i = end + 1
            continue
        if body[i] in ('union', 'struct') and body[i + 1] == '{':
            close = skip_to_matching(body, i + 1)
            end = body.index(';', close)
            names = [t for t in body[close + 1:end] if re.match(r'^[A-Za-z_]\w*$', t)]
            if names:
                for field in names:
                    fields.append({'name': field, 'typed': False, 'bits': False})
            else:
                parse_members(body[i + 2:close], fields, methods, owner)
            i = end + 1
            continue
        end = i
        while body[end] != ';':
            if body[end] == '{':
                # An inline member function ends with its body.
                end = skip_to_matching(body, end)
                break
            if body[end] in ('(', '['):
                end = skip_to_matching(body, end)
            end += 1
        member = body[i:end + 1] if body[end] == '}' else body[i:end]
        i = end + 1
        if member[0] == 'MAICORD_COM_UNUSED':
            methods.append({'name': member[2], 'unused': True})
            continue
        if member[0] == 'virtual':
            methods.append(parse_method(member, owner))
            continue
        if '{' in member or (member[0] in INLINE and '(' in member):
            # A non-virtual inline helper (Direct2D's reference overloads): it overloads a method.
            methods.append({'name': member[first_call_paren(member) - 1], 'unused': False, 'helper': True})
            continue
        if member[0] in ('static', 'template', 'friend', 'constexpr'):
            raise ParseError(f'{owner}: unsupported member {" ".join(member)}')
        bracketless = strip_arrays(member)
        if '(' in bracketless:
            # A function pointer field: TYPE (CALLCONV* name)(params).
            group = bracketless[bracketless.index('(') + 1:]
            names = [t for t in group[:group.index(')')] if re.match(r'^[A-Za-z_]\w*$', t)
                     and t not in CALLING_CONVENTIONS]
            fields.append({'name': names[-1], 'typed': True, 'bits': False})
            continue
        for declarator in split_top(member, ','):
            bits = ':' in declarator
            if bits:
                declarator = declarator[:declarator.index(':')]
            idents = [t for t in strip_arrays(declarator) if re.match(r'^[A-Za-z_]\w*$', t)]
            fields.append({'name': idents[-1], 'typed': True, 'bits': bits})


def parse_method(member, owner):
    member = member[1:]
    if member[-2:] != ['=', '0']:
        raise ParseError(f'{owner}: methods must be pure: {" ".join(member)}')
    member = member[:-2]
    qualifiers = []
    while member[-1] != ')':
        qualifiers.insert(0, member.pop())
    open_index = first_call_paren(member)
    name = member[open_index - 1]
    if skip_to_matching(member, open_index) != len(member) - 1:
        raise ParseError(f'{owner}::{name}: unsupported declarator')
    result = [t for t in without_declspecs(member[:open_index - 1]) if t not in CALLING_CONVENTIONS]
    return {'name': name, 'unused': False, 'result': ' '.join(result),
            'parameters': ', '.join(' '.join(p[:p.index('=')] if '=' in p else p)
                                    for p in split_top(member[open_index + 1:-1], ',')),
            'qualifiers': ' '.join(qualifiers)}


def render(headers):
    out = ['// Generated by tools/generate_sys_conformance.py; do not edit.', '#include "conformance_check.hpp"', '']
    # Every SDK header the headers mirror, each parsed once.
    sdk_includes = []
    for header, _ in headers:
        sdk_includes += [line for line in header.sdk_includes if line not in sdk_includes]
    guids = any(header.guids for header, _ in headers)
    capture = ('#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) namespace maicord_sdk_guid { '
               'constexpr GUID name = {l, w1, w2, {b1, b2, b3, b4, b5, b6, b7, b8}}; } extern "C" const GUID name')
    for index, line in enumerate(sdk_includes):
        out.append(line)
        if guids and index == 0:
            # The SDK's GUID values, as constants (its DEFINE_GUID only declares them).
            # <combaseapi.h> includes <guiddef.h>, which would restore DEFINE_GUID.
            out += ['#include <objbase.h>', '#undef DEFINE_GUID', capture]
    if guids:
        out += ['#undef DEFINE_GUID', capture.replace('maicord_sdk_guid', 'maicord_sdk_mirror')
                .replace(' extern "C" const GUID name', ' static_assert(true)')]
    out.append('')
    # Every existence check precedes every MaiCord header: one header may include
    # another, whose declarations would otherwise satisfy its checks.
    for header, _ in headers:
        for name in header.macros:
            out += [f'#if !defined({name})', f'#error "{name}: the SDK does not define it"', '#endif']
        # A compiler builtin (the interlocked and other intrinsics) has no header
        # declaration; redeclaring it with another type is still an error.
        for name in header.functions:
            out += [f'#if !__has_builtin({name})',
                    f'__if_not_exists(::{name}) {{ static_assert(false, "{name}: the SDK does not declare it"); }}',
                    '#endif']
        for name in header.typedefs + header.tags + header.guids:
            out.append(f'__if_not_exists(::{name}) {{ static_assert(false, "{name}: the SDK does not declare it"); }}')
    out += [''] + [f'#include "{relative[len("include/"):]}"' for _, relative in headers]
    out += ['', 'namespace maicord_sdk_mirror {}', 'namespace M = maicord_sdk_mirror;',
            'using namespace maicord_sys_check;', '']
    for header, _ in headers:
        out += comparisons(header)
    out.append('')
    return '\n'.join(out)


def comparisons(header):
    """The asserts comparing the types and values one header defines with the SDK's."""
    out = []
    for name in header.block_typedefs:
        out.append(f'static_assert(same_type<M::{name}, ::{name}>(), "{name}");')
    for name in header.guids:
        out.append(f'static_assert(same_guid(M::{name}, maicord_sdk_guid::{name}), "{name}");')
    for name in header.constants:
        out.append(f'static_assert(M::{name} == ::{name} && same_type<decltype(M::{name}), decltype(::{name})>(), '
                   f'"{name}");')
    for name in header.coclasses:
        out.append(f'static_assert(same_uuid<M::{name}, ::{name}>(), "{name}");')
    for enum in header.enums:
        name = enum['name']
        out.append(f'static_assert(sizeof(M::{name}) == sizeof(::{name}) && '
                   f'same_type<__underlying_type(M::{name}), __underlying_type(::{name})>(), "{name}");')
        scope = f'{name}::' if enum['scoped'] else ''
        for value in enum['enumerators']:
            out.append(f'static_assert(static_cast<__int64>(M::{scope}{value}) == '
                       f'static_cast<__int64>(::{scope}{value}), "{value}");')
    for record in header.records:
        name = record['name']
        out.append(f'static_assert(sizeof(M::{name}) == sizeof(::{name}) && alignof(M::{name}) == alignof(::{name}), '
                   f'"{name}");')
        out.append(f'static_assert(same_type<M::{name}, ::{name}>(), "{name}: tag");')
        if record['uuid']:
            out.append(f'static_assert(same_uuid<M::{name}, ::{name}>(), "{name}: IID");')
        if record['base']:
            base = record['base']
            out.append(f'static_assert(__is_base_of(M::{base}, M::{name}) && __is_base_of(::{base}, ::{name}), '
                       f'"{name}: base");')
        for field in record['fields']:
            member = f'{name}::{field["name"]}'
            if not field['bits']:
                out.append(f'static_assert(__builtin_offsetof(M::{name}, {field["name"]}) == '
                           f'__builtin_offsetof(::{name}, {field["name"]}), "{member}: offset");')
            if field['typed']:
                out.append(f'static_assert(same_type<decltype(M::{member}), decltype(::{member})>(), '
                           f'"{member}: type");')
        names = [m['name'] for m in record['methods'] if not m['unused']]
        for index, method in enumerate(record['methods']):
            if method.get('helper'):
                continue
            label = f'{name}::{method["name"]}'
            if method['unused']:
                out.append(f'static_assert(slot<&M::{name}::unused_{method["name"]}>() == '
                           f'slot<&::{name}::{method["name"]}>(), "{label}: slot");')
                continue
            qualifiers = f'{" " + method["qualifiers"] if method["qualifiers"] else ""}'
            signature = f'{method["result"]} (__stdcall ::{name}::*)({method["parameters"]}){qualifiers}'
            sdk = f'static_cast<{signature}>(&::{label})'
            ours = f'&M::{label}'
            if names.count(method['name']) > 1:
                # An overload: select ours by its signature, spelled in the mirror's scope.
                ours = f'static_cast<M::check_{name}_{index}>(&M::{label})'
                out.append(f'namespace maicord_sdk_mirror {{ using check_{name}_{index} = '
                           f'{method["result"]} (__stdcall {name}::*)({method["parameters"]}){qualifiers}; }}')
            out.append(f'static_assert(slot<{ours}>() == slot<{sdk}>(), "{label}: slot and signature");')
            out.append(f'static_assert(same_type<decltype({ours}), decltype({sdk})>(), "{label}: mirror signature");')
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--root', required=True, type=pathlib.Path)
    parser.add_argument('--headers', required=True, nargs='+', help='repository-relative header paths')
    parser.add_argument('--output', required=True, type=pathlib.Path)
    arguments = parser.parse_args()
    headers = []
    for relative in arguments.headers:
        try:
            headers.append((Header(arguments.root / relative), relative.replace('\\', '/')))
        except ParseError as error:
            sys.exit(f'{relative}: {error}')
    text = render(headers)
    if not arguments.output.exists() or arguments.output.read_text(encoding='utf-8') != text:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(text, encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()
