#!/usr/bin/env python3
"""Generates MaiCord's Wayland protocol bindings from the protocol XML in third_party/wayland.

    generate_wayland_protocols.py OUTPUT_DIRECTORY PROTOCOL.xml...

Writes wayland_protocols.hpp (typed enums, request wrappers and listener structs) and wayland_protocols.cpp (the
wl_interface tables), in namespace maicord::wayland. No wayland-scanner and no libwayland headers are involved: the
tables have libwayland's binary layout (maicord/platform/window/wayland/abi.hpp declares it) and every request is sent
through wl_proxy_marshal_array_flags (libwayland-client 1.20, the floor: Ubuntu 22.04), which also makes the object of a
constructor and destroys the proxy of a destructor in the same call. The library itself is dlopen()ed
(src/platform/window/wayland/client.cpp).

The output is deterministic: the same inputs give byte-identical files, and a file is rewritten only when its content
changes, so a build does not recompile what did not change.
"""

import os
import sys
import xml.etree.ElementTree as ET

CPP_KEYWORDS = frozenset("""
alignas alignof and and_eq asm auto bitand bitor bool break case catch char class compl concept const consteval
constexpr constinit const_cast continue co_await co_return co_yield decltype default delete do double dynamic_cast
else enum explicit export extern false float for friend goto if inline int long mutable namespace new noexcept not
not_eq nullptr operator or or_eq private protected public register reinterpret_cast requires return short signed
sizeof static static_assert static_cast struct switch template this thread_local throw true try typedef typeid
typename union unsigned using virtual void volatile wchar_t while xor xor_eq
""".split())

# Names the generated listener signatures and wrappers use themselves.
RESERVED_ARGUMENTS = frozenset(["self", "data", "listener", "args", "proxy", "created", "interface", "version"])

SIGNATURE_LETTER = {"int": "i", "uint": "u", "fixed": "f", "string": "s", "object": "o", "array": "a", "fd": "h",
                    "new_id": "n"}
ARGUMENT_FIELD = {"int": "i", "uint": "u", "fixed": "f", "string": "s", "object": "o", "array": "a", "fd": "h",
                  "new_id": "n"}

# Every interface name of the inputs, filled by main(): a request wrapper must not take the name of an interface.
INTERFACE_NAMES = set()


def fail(message):
    sys.stderr.write("generate_wayland_protocols.py: " + message + "\n")
    sys.exit(1)


def identifier(name):
    if name in CPP_KEYWORDS or name in RESERVED_ARGUMENTS:
        return name + "_"
    return name


class Argument:
    def __init__(self, element):
        self.name = element.get("name")
        self.type = element.get("type")
        self.interface = element.get("interface")
        self.nullable = element.get("allow-null") == "true"
        if self.type not in SIGNATURE_LETTER:
            fail("unknown argument type " + str(self.type))
        if self.type == "new_id" and self.interface is None:
            # wl_registry.bind: the interface name and version travel on the wire before the new id.
            pass

    @property
    def untyped_new_id(self):
        return self.type == "new_id" and self.interface is None


class Message:
    def __init__(self, element, kind, interface_name):
        self.kind = kind
        self.name = element.get("name")
        self.since = int(element.get("since", "1"))
        self.destructor = element.get("type") == "destructor"
        self.arguments = [Argument(a) for a in element.findall("arg")]
        description = element.find("description")
        self.summary = description.get("summary") if description is not None else None
        if sum(1 for a in self.arguments if a.type == "new_id") > 1 and kind == "request":
            fail(interface_name + "." + self.name + " has two new_id arguments")

    def signature(self):
        text = str(self.since) if self.since > 1 else ""
        for a in self.arguments:
            if a.nullable:
                text += "?"
            if a.untyped_new_id:
                text += "su"
            text += SIGNATURE_LETTER[a.type]
        return text

    def type_entries(self):
        """One entry per signature letter (version digits and '?' excluded): the interface or None."""
        entries = []
        for a in self.arguments:
            if a.untyped_new_id:
                entries += [None, None, None]
            elif a.type in ("object", "new_id"):
                entries.append(a.interface)
            else:
                entries.append(None)
        return entries


class Enum:
    def __init__(self, element):
        self.name = element.get("name")
        self.bitfield = element.get("bitfield") == "true"
        self.entries = []
        for entry in element.findall("entry"):
            self.entries.append((entry.get("name"), int(entry.get("value"), 0)))


class Interface:
    def __init__(self, element):
        self.name = element.get("name")
        self.version = int(element.get("version"))
        self.requests = [Message(e, "request", self.name) for e in element.findall("request")]
        self.events = [Message(e, "event", self.name) for e in element.findall("event")]
        self.enums = [Enum(e) for e in element.findall("enum")]
        seen = set()
        for message in self.requests + self.events:
            if message.name in seen:
                fail(self.name + " has a request and an event named " + message.name)
            seen.add(message.name)


class Protocol:
    def __init__(self, path):
        root = ET.parse(path).getroot()
        self.path = path
        self.name = root.get("name")
        copyright_element = root.find("copyright")
        self.copyright = copyright_element.text.strip() if copyright_element is not None else ""
        self.interfaces = [Interface(e) for e in root.findall("interface")]


def argument_cpp_type(argument):
    if argument.type in ("int", "fixed", "fd"):
        return "i32"
    if argument.type == "uint":
        return "u32"
    if argument.type == "string":
        return "const char*"
    if argument.type == "array":
        return "Array*"
    if argument.interface:
        return argument.interface + "*"
    return "void*"


def comment_lines(text, prefix="// "):
    return "".join(prefix + line.rstrip() + "\n" if line.strip() else prefix.rstrip() + "\n"
                   for line in text.splitlines())


def banner(protocol):
    text = "Protocol " + protocol.name + " (" + os.path.basename(protocol.path) + ")\n"
    if protocol.copyright:
        text += "\n" + "\n".join(line.strip() for line in protocol.copyright.splitlines())
    return comment_lines(text)


def emit_enums(out, interface):
    for enum in interface.enums:
        name = interface.name + "_" + enum.name
        out.append("enum " + name + " : u32 {\n")
        for entry_name, value in enum.entries:
            out.append("    " + name + "_" + entry_name + " = " + str(value) + ",\n")
        out.append("};\n")


def request_parameters_and_body(interface, opcode, message):
    parameters = [interface.name + "* self"]
    wire = []  # lines assigning args[i]
    index = 0
    result = "void"
    constructor = None
    for a in message.arguments:
        if a.untyped_new_id:
            parameters += ["const Interface* interface", "u32 version"]
            wire.append("    args[%d].s = interface->name;" % index)
            wire.append("    args[%d].u = version;" % (index + 1))
            wire.append("    args[%d].o = nullptr;" % (index + 2))
            index += 3
            result = "void*"
            constructor = ("interface", "version")
            continue
        field = ARGUMENT_FIELD[a.type]
        if a.type == "new_id":
            wire.append("    args[%d].o = nullptr;" % index)
            result = a.interface + "*"
            constructor = ("&" + a.interface + "_interface", "g_client.proxy_get_version(proxy)")
        else:
            name = identifier(a.name)
            parameters.append(argument_cpp_type(a) + " " + name)
            value = "reinterpret_cast<void*>(" + name + ")" if a.type == "object" else name
            wire.append("    args[%d].%s = %s;" % (index, field, value))
        index += 1
    return parameters, wire, index, result, constructor


def emit_request(out, interface, opcode, message):
    parameters, wire, count, result, constructor = request_parameters_and_body(interface, opcode, message)
    function = interface.name + "_" + message.name
    if function in INTERFACE_NAMES:
        function += "_request"  # a request named like the interface it creates (wp_presentation.feedback)
    if message.summary:
        out.append("// " + message.summary + "\n")
    if message.since > 1:
        out.append("inline constexpr u32 %s_since = %d;\n" % (function, message.since))
    out.append("inline %s %s(%s) noexcept {\n" % (result, function, ", ".join(parameters)))
    out.append("    Proxy* proxy = reinterpret_cast<Proxy*>(self);\n")
    if count:
        out.append("    Argument args[%d] = {};\n" % count)
    arguments = "args" if count else "nullptr"
    out.extend(line + "\n" for line in wire)
    flags = "kMarshalFlagDestroy" if message.destructor else "0"
    if constructor:
        out.append("    return reinterpret_cast<%s>(g_client.proxy_marshal_array_flags(proxy, %d, %s, %s, %s, %s));\n"
                   % (result, opcode, constructor[0], constructor[1], flags, arguments))
    else:
        out.append("    g_client.proxy_marshal_array_flags(proxy, %d, nullptr, 0, %s, %s);\n" % (opcode, flags, arguments))
    out.append("}\n")


def emit_listener(out, interface):
    if not interface.events:
        return
    out.append("struct %s_listener {\n" % interface.name)
    for event in interface.events:
        parameters = ["void* data", interface.name + "* self"]
        for a in event.arguments:
            parameters.append(argument_cpp_type(a) + " " + identifier(a.name))
        if event.summary:
            out.append("    // " + event.summary + "\n")
        if event.since > 1:
            out.append("    // since version %d\n" % event.since)
        out.append("    void (*%s)(%s) noexcept;\n" % (event.name, ", ".join(parameters)))
    out.append("};\n")
    for event in interface.events:
        if event.since > 1:
            out.append("inline constexpr u32 %s_%s_since = %d;\n" % (interface.name, event.name, event.since))
    out.append("inline i32 %s_add_listener(%s* self, const %s_listener* listener, void* data) noexcept {\n"
               % (interface.name, interface.name, interface.name))
    out.append("    return g_client.proxy_add_listener(reinterpret_cast<Proxy*>(self),\n")
    out.append("                                       reinterpret_cast<void (**)()>(const_cast<%s_listener*>(listener)), data);\n"
               % interface.name)
    out.append("}\n")


def generate_header(protocols):
    out = []
    out.append("// Generated by tools/generate_wayland_protocols.py from third_party/wayland/*.xml; do not edit.\n")
    out.append("#pragma once\n\n")
    out.append('#include "maicord/platform/window/wayland/abi.hpp"\n\n')
    out.append("namespace maicord::wayland {\n\n")
    out.append("// Every interface: the object type the wrappers take, and the table libwayland-client marshals by.\n")
    for protocol in protocols:
        for interface in protocol.interfaces:
            out.append("struct %s;\n" % interface.name)
    out.append("\n")
    for protocol in protocols:
        for interface in protocol.interfaces:
            out.append("extern const Interface %s_interface;\n" % interface.name)
    out.append("\n// All interfaces in file order, for tests and diagnostics.\n")
    out.append("extern const Interface* const g_protocol_interfaces[];\n")
    out.append("extern const u32 g_protocol_interface_count;\n")
    for protocol in protocols:
        out.append("\n// " + "-" * 116 + "\n")
        out.append(banner(protocol))
        for interface in protocol.interfaces:
            out.append("\n// Interface %s, version %d.\n" % (interface.name, interface.version))
            emit_enums(out, interface)
            for opcode, message in enumerate(interface.requests):
                emit_request(out, interface, opcode, message)
            emit_listener(out, interface)
            if not any(m.name == "destroy" for m in interface.requests):
                # What wayland-scanner's headers add: interfaces without a destroy request are freed with the proxy.
                out.append("inline void %s_destroy(%s* self) noexcept {\n" % (interface.name, interface.name))
                out.append("    g_client.proxy_destroy(reinterpret_cast<Proxy*>(self));\n}\n")
    out.append("\n} // namespace maicord::wayland\n")
    return "".join(out)


def generate_source(protocols):
    defined = set(i.name for p in protocols for i in p.interfaces)
    out = []
    out.append("// Generated by tools/generate_wayland_protocols.py from third_party/wayland/*.xml; do not edit.\n")
    out.append('#include "wayland_protocols.hpp"\n\n')
    out.append("namespace maicord::wayland {\n")
    for protocol in protocols:
        out.append("\n// " + "-" * 116 + "\n")
        out.append(banner(protocol))
        for interface in protocol.interfaces:
            out.append("\nnamespace {\n")
            for kind, messages in (("request", interface.requests), ("event", interface.events)):
                for message in messages:
                    entries = message.type_entries()
                    if not entries:
                        continue
                    for entry in entries:
                        if entry is not None and entry not in defined:
                            fail("%s.%s names the interface %s that no input file defines"
                                 % (interface.name, message.name, entry))
                    items = ", ".join("&%s_interface" % e if e else "nullptr" for e in entries)
                    out.append("const Interface* const %s_%s_%s_types[%d] = {%s};\n"
                               % (interface.name, kind, message.name, len(entries), items))
            for kind, messages in (("request", interface.requests), ("event", interface.events)):
                if not messages:
                    continue
                out.append("const Message %s_%ss[%d] = {\n" % (interface.name, kind, len(messages)))
                for message in messages:
                    types = ("%s_%s_%s_types" % (interface.name, kind, message.name)) if message.type_entries() \
                        else "nullptr"
                    out.append('    {"%s", "%s", %s},\n' % (message.name, message.signature(), types))
                out.append("};\n")
            out.append("} // namespace\n")
            out.append("const Interface %s_interface = {\"%s\", %d, %d, %s, %d, %s};\n" % (
                interface.name, interface.name, interface.version,
                len(interface.requests), (interface.name + "_requests") if interface.requests else "nullptr",
                len(interface.events), (interface.name + "_events") if interface.events else "nullptr"))
    out.append("\nconst Interface* const g_protocol_interfaces[] = {\n")
    count = 0
    for protocol in protocols:
        for interface in protocol.interfaces:
            out.append("    &%s_interface,\n" % interface.name)
            count += 1
    out.append("};\n")
    out.append("const u32 g_protocol_interface_count = %d;\n" % count)
    out.append("\n} // namespace maicord::wayland\n")
    return "".join(out)


def write_if_changed(path, text):
    try:
        with open(path, "r", encoding="utf-8", newline="") as existing:
            if existing.read() == text:
                return
    except FileNotFoundError:
        pass
    with open(path, "w", encoding="utf-8", newline="\n") as output:
        output.write(text)


def main(argv):
    if len(argv) < 3:
        fail("usage: generate_wayland_protocols.py OUTPUT_DIRECTORY PROTOCOL.xml...")
    protocols = [Protocol(path) for path in argv[2:]]
    names = {}
    INTERFACE_NAMES.update(i.name for p in protocols for i in p.interfaces)
    for protocol in protocols:
        for interface in protocol.interfaces:
            if interface.name in names:
                fail("interface %s is defined by %s and %s" % (interface.name, names[interface.name], protocol.path))
            names[interface.name] = protocol.path
    os.makedirs(argv[1], exist_ok=True)
    write_if_changed(os.path.join(argv[1], "wayland_protocols.hpp"), generate_header(protocols))
    write_if_changed(os.path.join(argv[1], "wayland_protocols.cpp"), generate_source(protocols))


if __name__ == "__main__":
    main(sys.argv)
