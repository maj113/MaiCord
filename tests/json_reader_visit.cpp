// A visit of a validated value reads members from the structural index alone. It must give exactly what the
// full validation of the same text gives, whatever the whitespace, nesting, order of the lookups or rejections.
#include "maicord/core/json_reader.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

struct Dump {
    char text[65536];
    u32 used;
    void put(char c) noexcept {
        if (used < sizeof(text)) text[used++] = c;
    }
    void put(const char* data, u32 bytes) noexcept {
        for (u32 i = 0; i < bytes; ++i) put(data[i]);
    }
    void number(u32 value) noexcept {
        char digits[10];
        u32 count = 0;
        do digits[count++] = static_cast<char>('0' + value % 10);
        while (value /= 10);
        while (count) put(digits[--count]);
    }
};

struct Walk {
    const JsonIndexedDocument* document;
    Dump* dump;
    bool trusted;
    u32 depth_limit;
    u32 reject_at;
    u32 calls;
};

bool walk_value(Walk* walk, JsonValue value) noexcept;

bool walk_member(void* context, JsonString name, JsonValue value) noexcept {
    auto* walk = static_cast<Walk*>(context);
    walk->dump->put(name.encoded.data, name.encoded.bytes);
    walk->dump->put(name.has_escapes ? '!' : '=');
    walk->dump->number(name.source_offset);
    return walk_value(walk, value);
}

bool walk_element(void* context, u32 index, JsonValue value) noexcept {
    auto* walk = static_cast<Walk*>(context);
    walk->dump->put('#');
    walk->dump->number(index);
    return walk_value(walk, value);
}

bool walk_value(Walk* walk, JsonValue value) noexcept {
    Dump& dump = *walk->dump;
    dump.number(static_cast<u32>(value.kind));
    dump.put(':');
    dump.number(value.source_offset);
    dump.put(':');
    dump.put(value.source.data, value.source.bytes);
    dump.put(';');
    if (walk->calls++ == walk->reject_at) return false;
    if (value.kind == JsonValueKind::object || value.kind == JsonValueKind::array) {
        // The trusted walk keeps the reader's own flag; the other revalidates every nested value.
        if (!walk->trusted) value.validated = false;
        const bool object = value.kind == JsonValueKind::object;
        dump.put(object ? '{' : '[');
        const JsonReadResult result = object ? json_visit_indexed_object(*walk->document, value, walk->depth_limit,
                                                                         walk_member, walk)
                                             : json_visit_indexed_array(*walk->document, value, walk->depth_limit,
                                                                        walk_element, walk);
        dump.put('(');
        dump.number(static_cast<u32>(result.error));
        dump.put(',');
        dump.number(result.member_count);
        dump.put(',');
        dump.number(result.error_offset);
        dump.put(')');
    }
    return true;
}

// The dump of the root object's whole tree, nested values read trusted or fully validated.
bool walk_document(Utf8View text, bool trusted, u32 depth_limit, u32 reject_at, Dump* dump) noexcept {
    static u32 index[8192];
    const JsonScanResult scan = json_scan_structurals_avx1(text, {index, 8192});
    if (!scan.ok()) return false;
    const JsonIndexedDocument document{text, {index, scan.count}};
    Walk walk{&document, dump, trusted, depth_limit, reject_at, 0};
    dump->used = 0;
    const JsonReadResult root =
        json_visit_root_object(text, document.structurals, depth_limit, walk_member, &walk);
    dump->put('(');
    dump->number(static_cast<u32>(root.error));
    dump->put(',');
    dump->number(root.member_count);
    dump->put(',');
    dump->number(root.error_offset);
    dump->put(')');
    return true;
}

bool same_dump(const Dump& left, const Dump& right) noexcept {
    return left.used == right.used && left.used < sizeof(left.text) &&
           bytes_equal(left.text, right.text, left.used);
}

bool agree(Utf8View text, u32 depth_limit, u32 reject_at) noexcept {
    static Dump trusted, checked;
    return walk_document(text, true, depth_limit, reject_at, &trusted) &&
           walk_document(text, false, depth_limit, reject_at, &checked) && same_dump(trusted, checked);
}

// A member list of `count` objects, each with a nested object and array.
u32 make_items(char* out, u32 items) noexcept {
    u32 used = 0;
    const auto put = [&](const char* text) noexcept {
        for (; *text; ++text) out[used++] = *text;
    };
    put("{\"items\":[");
    for (u32 i = 0; i < items; ++i) {
        if (i) put(",");
        put("{\"id\":\"");
        out[used++] = static_cast<char>('0' + i % 10);
        put("\",\"n\":[1,-2.5e3,true,false,null,{\"k\":\"v\\n\"}],\"o\":{\"p\":{\"q\":[]}},\"s\":\"\\u00e9\xc3\xa9\"}");
    }
    put("],\"tail\":0}");
    return used;
}

struct Collected {
    JsonValue values[200];
    u32 count;
};

bool collect_element(void* context, u32, JsonValue value) noexcept {
    auto* collected = static_cast<Collected*>(context);
    if (collected->count == 200) return false;
    collected->values[collected->count++] = value;
    return true;
}

bool find_items(void* context, JsonString name, JsonValue value) noexcept {
    if (json_string_equals_ascii(name, utf8_literal("items"))) *static_cast<JsonValue*>(context) = value;
    return true;
}

// The scalar reference of the scanner: unescaped quotes and the punctuation outside strings.
u32 reference_offsets(Utf8View text, u32* out) noexcept {
    u32 count = 0, backslashes = 0;
    bool inside = false;
    for (u32 i = 0; i < text.bytes; ++i) {
        const char c = text.data[i];
        if (c == '"' && !(backslashes & 1)) {
            out[count++] = i;
            inside = !inside;
        } else if (!inside && (c == '{' || c == '}' || c == '[' || c == ']' || c == ':' || c == ',')) {
            out[count++] = i;
        }
        backslashes = c == '\\' ? backslashes + 1 : 0;
    }
    return count;
}

// The scan at every capacity around the structural count, with guard words after the output.
bool scan_fits_every_capacity(Utf8View text) noexcept {
    static u32 expected[1024];
    static u32 buffer[1024 + 80];
    const u32 total = reference_offsets(text, expected);
    for (u32 capacity = 0; capacity <= total + 80 && capacity + 1 < 1024 + 80 - 70; ++capacity) {
        if (capacity > 8 && capacity + 8 < total && capacity % 7 != 0) continue;
        for (u32 i = 0; i < 1024 + 80; ++i) buffer[i] = 0xA5A5A5A5u;
        const JsonScanResult scan = json_scan_structurals_avx1(text, {buffer, capacity});
        for (u32 i = capacity; i < 1024 + 80; ++i)
            if (buffer[i] != 0xA5A5A5A5u) return false;
        if (capacity >= total) {
            if (!scan.ok() || scan.count != total) return false;
        } else if (scan.error != JsonScanError::output_overflow || scan.count != capacity ||
                   scan.error_offset != expected[capacity]) {
            return false;
        }
        for (u32 i = 0; i < scan.count; ++i)
            if (buffer[i] != expected[i]) return false;
    }
    return true;
}

} // namespace

void test_json_reader_visit(void (*check)(bool, const char*) noexcept) noexcept {
    const Utf8View documents[]{
        utf8_literal(R"({"a":1,"b":[true,false,null,-1.5e+3,"x\ny"],"c":{"d":{"e":[[],{}]}},"f":"\u00e9é"})"),
        utf8_literal("{ \"a\" : 1 , \"b\" : [ 1 , 2 ] , \"c\" : { } , \"d\" : [ ] , \"e\" : \"s\" }"),
        utf8_literal("{\t\"a\"\n:\r1\t,\n\"b\"\t:\n[\r1\n,\t2\r]\n,\t\"c\"\r:\t{\n\"x\"\r:\n[\t{\r}\n]\t}\r}"),
        utf8_literal(R"({"a":12 ,"b":true ,"c":null ,"d":"x" ,"e":[1 ,2 ,3 ] ,"f":{"g":-0 }})"),
        utf8_literal(R"({"":"","a":[""],"bb":{"":{}},"\u0061":1,"a\"b":2,"q\\":3})"),
        utf8_literal(R"({"a":{"b":{"c":{"d":{"e":{"f":{"g":{"h":[[[[[1]]]]]}}}}}}}})"),
        utf8_literal(R"({"list":[{"id":1},{"id":2},[3],"s",4,true,null,{"a":[{"b":{}}]}]})"),
    };
    for (const Utf8View text : documents) {
        check(agree(text, 64, ~0u), "json trusted visit agrees with full validation on nested values");
        for (u32 reject = 0; reject < 12; ++reject)
            check(agree(text, 64, reject), "json trusted visit reports a rejecting visitor like full validation");
        for (u32 depth = 1; depth <= 14; ++depth)
            check(agree(text, depth, ~0u), "json trusted visit applies the depth limit like full validation");
    }

    static char items[8192];
    const Utf8View big{items, make_items(items, 24)};
    check(agree(big, 64, ~0u) && agree(big, 5, ~0u) && agree(big, 6, 40), "json trusted visit agrees on a long list");

    // Lookups in reverse and shuffled order, after other visits moved the document's lookup hint.
    static u32 index[8192];
    const JsonScanResult scan = json_scan_structurals_avx1(big, {index, 8192});
    const JsonIndexedDocument document{big, {index, scan.count}};
    JsonValue list{};
    static Collected collected;
    collected.count = 0;
    bool ok = scan.ok() && json_visit_root_object(big, document.structurals, 64, find_items, &list).ok() &&
              json_visit_indexed_array(document, list, 64, collect_element, &collected).ok() &&
              collected.count == 24;
    u32 state = 12345;
    for (u32 round = 0; ok && round < 4; ++round) {
        for (u32 step = 0; ok && step < collected.count; ++step) {
            state = state * 1664525u + 1013904223u;
            const u32 pick = round == 0 ? collected.count - 1 - step : round == 1 ? step : (state >> 8) % collected.count;
            static Dump trusted, checked;
            JsonValue value = collected.values[pick];
            Walk first{&document, &trusted, true, 64, ~0u, 0};
            trusted.used = 0;
            const JsonReadResult a = json_visit_indexed_object(document, value, 64, walk_member, &first);
            value.validated = false;
            Walk second{&document, &checked, false, 64, ~0u, 0};
            checked.used = 0;
            const JsonReadResult b = json_visit_indexed_object(document, value, 64, walk_member, &second);
            ok = a.ok() && b.ok() && a.member_count == b.member_count && same_dump(trusted, checked);
        }
    }
    check(ok, "json indexed visits find values in any order of lookup");

    check(scan_fits_every_capacity(utf8_literal(R"({"a":[1,2,{"b":"c"}],"d":"e\"f"})")),
          "json scan fills small outputs exactly");
    static char dense[300];
    for (u32 i = 0; i < sizeof(dense); ++i) dense[i] = "[{,:"[i & 3];
    check(scan_fits_every_capacity({dense, sizeof(dense)}), "json scan fills outputs when every byte is structural");
    static char quotes[300];
    for (u32 i = 0; i < sizeof(quotes); ++i) quotes[i] = i % 5 == 0 ? '"' : i % 5 == 3 ? ',' : 'x';
    check(scan_fits_every_capacity({quotes, sizeof(quotes)}), "json scan fills outputs around string edges");
}

} // namespace maicord
