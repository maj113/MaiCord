#include "maicord/core/heap.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/platform/dbus/connection.hpp"
#include "maicord/platform/dbus/wire.hpp"
#include "../../support/test_output.hpp"

// The D-Bus marshalling and address layers without a bus: byte-exact vectors worked out from the specification, round
// trips of the whole type system, header parsing, hostile input (truncated, mutated, overlong) and the address forms.

namespace maicord {
namespace {

using namespace dbus;

u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

bool text_equal(const char* a, const char* b) noexcept {
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}

bool bytes_are(const Builder& b, const u8* expected, u32 count) noexcept {
    return !b.failed && b.size == count && bytes_equal(b.data, expected, count);
}

void test_vectors() noexcept {
    {
        Builder b;
        builder_init(&b);
        builder_byte(&b, 1);
        builder_u32(&b, 2);
        const u8 expected[] = {1, 0, 0, 0, 2, 0, 0, 0};
        check(bytes_are(b, expected, 8) && text_equal(b.signature, "yu"), "a byte pads to the u32's alignment");
        builder_release(&b);
    }
    {
        Builder b;
        builder_init(&b);
        builder_string(&b, "hi");
        builder_signature(&b, "a{sv}");
        const u8 expected[] = {2, 0, 0, 0, 'h', 'i', 0, 5, 'a', '{', 's', 'v', '}', 0};
        check(bytes_are(b, expected, sizeof(expected)) && text_equal(b.signature, "sg"), "string and signature layout");
        builder_release(&b);
    }
    {
        Builder b;
        builder_init(&b);
        builder_open_array(&b, "t");
        builder_close_array(&b);
        const u8 expected[] = {0, 0, 0, 0, 0, 0, 0, 0};
        check(bytes_are(b, expected, 8) && text_equal(b.signature, "at"),
              "an empty array of 8-aligned elements still pads to 8");
        builder_release(&b);
    }
    {
        // a{sv} {"k": 5u}
        Builder b;
        builder_init(&b);
        builder_open_array(&b, "{sv}");
        builder_open_entry(&b);
        builder_string(&b, "k");
        builder_open_variant(&b, "u");
        builder_u32(&b, 5);
        builder_close_variant(&b);
        builder_close_entry(&b);
        builder_close_array(&b);
        const u8 expected[] = {16, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 'k', 0, 1, 'u', 0, 0, 0, 0, 5, 0, 0, 0};
        check(bytes_are(b, expected, sizeof(expected)) && text_equal(b.signature, "a{sv}"), "a dictionary entry with a variant");
        builder_release(&b);
    }
    {
        Builder b;
        builder_init(&b);
        builder_bool(&b, true);
        builder_i16(&b, -2);
        builder_i64(&b, -1);
        builder_double(&b, 1.0);
        const u8 expected[] = {1, 0, 0, 0, 0xFE, 0xFF, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                               0, 0, 0, 0, 0, 0, 0xF0, 0x3F};
        check(bytes_are(b, expected, sizeof(expected)), "fixed-size types, little endian");
        builder_release(&b);
    }
    {
        // What the client reads but never writes (settings values, shortcut timestamps): q and t from wire bytes.
        const u8 bytes[] = {0x34, 0x12, 0, 0, 0, 0, 0, 0, 8, 7, 6, 5, 4, 3, 2, 1};
        Reader r;
        reader_init(&r, bytes, sizeof(bytes), "qt", nullptr, 0);
        u16 q = 0;
        u64 t = 0;
        check(reader_u16(&r, &q) && q == 0x1234 && reader_u64(&r, &t) && t == 0x0102030405060708ull && !reader_more(r),
              "q and t read little endian, t aligned to 8");
    }
    {
        // A struct is 8-aligned: (yy) after one byte.
        Builder b;
        builder_init(&b);
        builder_byte(&b, 9);
        builder_open_struct(&b, "(yy)");
        builder_byte(&b, 1);
        builder_byte(&b, 2);
        builder_close_struct(&b);
        const u8 expected[] = {9, 0, 0, 0, 0, 0, 0, 0, 1, 2};
        check(bytes_are(b, expected, sizeof(expected)) && text_equal(b.signature, "y(yy)"), "a struct aligns to 8");
        builder_release(&b);
    }
}

void test_builder_errors() noexcept {
    Builder b;
    builder_init(&b);
    builder_string(&b, "a\0b", 3);
    check(b.failed, "a string with a NUL is refused");
    builder_release(&b);

    builder_init(&b);
    builder_open_array(&b, "s");
    check(!builder_ok(b), "an open container is not a complete body");
    builder_close_struct(&b);
    check(b.failed, "closing the wrong container fails the builder");
    builder_release(&b);

    builder_init(&b);
    builder_open_array(&b, "q(");
    check(b.failed, "an invalid element signature is refused");
    builder_release(&b);

    builder_init(&b);
    for (u32 i = 0; i < kMaxDepth + 2; ++i)
        builder_open_array(&b, "s");
    check(b.failed, "nesting is bounded");
    builder_release(&b);

    check(signature_valid("") && signature_valid("a{sv}") && signature_valid("(oayays)") && signature_valid("a{sa{sv}}") &&
              signature_valid("aaai") && signature_valid("ssa(ii)v") && signature_valid("h"),
          "valid signatures");
    check(!signature_valid("(") && !signature_valid("()") && !signature_valid("a") && !signature_valid("{sv}x}") &&
              !signature_valid("a{vs}") && !signature_valid("(i") && !signature_valid("z"),
          "invalid signatures");
}

// Everything at once: written, then read back and compared.
void write_everything(Builder* b) noexcept {
    builder_byte(b, 200);
    builder_bool(b, true);
    builder_i16(b, -1234);
    builder_i32(b, -123456789);
    builder_u32(b, 4000000000u);
    builder_i64(b, -1234567890123ll);
    builder_double(b, -2.5);
    builder_string(b, "héllo");
    builder_object_path(b, "/org/test/Path");
    builder_signature(b, "a{sv}");
    builder_open_array(b, "(sa{sv}ai)");
    for (u32 i = 0; i < 3; ++i) {
        builder_open_struct(b, "(sa{sv}ai)");
        builder_string(b, i == 0 ? "zero" : i == 1 ? "one" : "two");
        builder_open_array(b, "{sv}");
        for (u32 j = 0; j <= i; ++j) {
            builder_open_entry(b);
            builder_string(b, j == 0 ? "a" : j == 1 ? "bb" : "ccc");
            if (j == 1) {
                builder_open_variant(b, "as");
                builder_open_array(b, "s");
                builder_string(b, "x");
                builder_string(b, "yz");
                builder_close_array(b);
                builder_close_variant(b);
            } else {
                builder_open_variant(b, "v");
                builder_open_variant(b, "x");
                builder_i64(b, 100 + j);
                builder_close_variant(b);
                builder_close_variant(b);
            }
            builder_close_entry(b);
        }
        builder_close_array(b);
        builder_open_array(b, "i");
        for (i32 k = 0; k < static_cast<i32>(i * 3); ++k)
            builder_i32(b, k - 4);
        builder_close_array(b);
        builder_close_struct(b);
    }
    builder_close_array(b);
    const u8 blob[5] = {1, 2, 3, 4, 5};
    builder_byte_array(b, blob, 5);
    builder_byte_array(b, nullptr, 0);
    builder_open_array(b, "ay");
    builder_close_array(b);
}

constexpr const char* kEverything = "ybniuxdsoga(sa{sv}ai)ayayaay";

bool read_everything(Reader* r) noexcept {
    u8 y;
    bool bo;
    i16 n;
    i32 i;
    u32 u;
    i64 x;
    double d;
    const char* s;
    u32 length;
    bool ok = reader_byte(r, &y) && y == 200 && reader_bool(r, &bo) && bo && reader_i16(r, &n) && n == -1234 &&
              reader_i32(r, &i) && i == -123456789 && reader_u32(r, &u) && u == 4000000000u && reader_i64(r, &x) &&
              x == -1234567890123ll && reader_double(r, &d) && d == -2.5;
    ok = ok && reader_string(r, &s, &length) && length == 6 && text_equal(s, "h\xC3\xA9llo");
    ok = ok && reader_string(r, &s, &length) && text_equal(s, "/org/test/Path");
    ok = ok && reader_string(r, &s, &length) && text_equal(s, "a{sv}");
    ok = ok && reader_type(*r) == 'a' && reader_enter_array(r);
    for (u32 index = 0; ok && index < 3; ++index) {
        ok = reader_more(*r) && reader_enter_struct(r) && reader_string(r, &s, &length) &&
             text_equal(s, index == 0 ? "zero" : index == 1 ? "one" : "two") && reader_enter_array(r);
        for (u32 j = 0; ok && j <= index; ++j) {
            const char* inner = nullptr;
            ok = reader_more(*r) && reader_enter_struct(r) && reader_string(r, &s, &length) &&
                 text_equal(s, j == 0 ? "a" : j == 1 ? "bb" : "ccc") && reader_enter_variant(r, &inner);
            if (!ok)
                break;
            if (j == 1) {
                ok = text_equal(inner, "as") && reader_enter_array(r) && reader_string(r, &s, nullptr) && text_equal(s, "x") &&
                     reader_string(r, &s, nullptr) && text_equal(s, "yz") && !reader_more(*r) && reader_leave(r);
            } else {
                const char* deeper = nullptr;
                ok = text_equal(inner, "v") && reader_enter_variant(r, &deeper) && text_equal(deeper, "x") &&
                     reader_i64(r, &x) && x == 100 + j && reader_leave(r);
            }
            ok = ok && reader_leave(r) && reader_leave(r);  // the variant, the entry
        }
        ok = ok && !reader_more(*r) && reader_leave(r) && reader_enter_array(r);
        for (i32 k = 0; ok && k < static_cast<i32>(index * 3); ++k)
            ok = reader_more(*r) && reader_i32(r, &i) && i == k - 4;
        ok = ok && !reader_more(*r) && reader_leave(r) && reader_leave(r);
    }
    ok = ok && !reader_more(*r) && reader_leave(r);
    const u8* bytes = nullptr;
    u32 count = 0;
    ok = ok && reader_byte_array(r, &bytes, &count) && count == 5 && bytes[0] == 1 && bytes[4] == 5;
    ok = ok && reader_byte_array(r, &bytes, &count) && count == 0;
    ok = ok && reader_enter_array(r) && !reader_more(*r) && reader_leave(r);
    return ok && reader_type(*r) == 0 && !r->failed;
}

void test_round_trip() noexcept {
    Builder b;
    builder_init(&b);
    write_everything(&b);
    check(builder_ok(b) && text_equal(b.signature, kEverything), "the builder's signature matches what was appended");
    Reader r;
    reader_init(&r, b.data, b.size, b.signature, nullptr, 0);
    check(read_everything(&r), "every type reads back");

    // The same body walked with skip alone, and with a partial read followed by leave.
    reader_init(&r, b.data, b.size, b.signature, nullptr, 0);
    u32 values = 0;
    while (reader_more(r) && reader_skip(&r))
        ++values;
    check(!r.failed && values == 14 && r.position == b.size, "skip walks the whole body");
    reader_init(&r, b.data, b.size, b.signature, nullptr, 0);
    for (u32 i = 0; i < 10; ++i)
        reader_skip(&r);
    const u32 ignored = 0;
    const bool partial = reader_enter_array(&r) && reader_enter_struct(&r) && reader_leave(&r) && reader_more(r) &&
                         reader_leave(&r) && reader_type(r) == 'a';
    (void)ignored;
    check(partial && !r.failed, "leaving a container skips what was not read");

    // Wrong-type reads fail and stay failed.
    reader_init(&r, b.data, b.size, b.signature, nullptr, 0);
    u32 wrong;
    check(!reader_u32(&r, &wrong) && r.failed && !reader_more(r), "a read of the wrong type fails the reader for good");
    builder_release(&b);
}

// Skipping a variant leaves the signature on the value after it (OpenSession's reply is "vo").
void test_variant_skip() noexcept {
    Builder b;
    builder_init(&b);
    builder_open_variant(&b, "as");
    builder_open_array(&b, "s");
    builder_string(&b, "one");
    builder_close_array(&b);
    builder_close_variant(&b);
    builder_object_path(&b, "/after");
    builder_u32(&b, 7);
    Reader r;
    reader_init(&r, b.data, b.size, b.signature, nullptr, 0);
    const char* path = nullptr;
    u32 number = 0;
    check(reader_skip(&r) && reader_string(&r, &path, nullptr) && text_equal(path, "/after") && reader_u32(&r, &number) &&
              number == 7 && !reader_more(r),
          "a skipped variant is followed by the next value");
    // The same inside an array of structs.
    builder_release(&b);
    builder_init(&b);
    builder_open_array(&b, "(vs)");
    for (u32 i = 0; i < 2; ++i) {
        builder_open_struct(&b, "(vs)");
        builder_open_variant(&b, "u");
        builder_u32(&b, i);
        builder_close_variant(&b);
        builder_string(&b, i ? "second" : "first");
        builder_close_struct(&b);
    }
    builder_close_array(&b);
    reader_init(&r, b.data, b.size, b.signature, nullptr, 0);
    bool ok = reader_enter_array(&r);
    const char* names[2] = {};
    for (u32 i = 0; ok && i < 2; ++i)
        ok = reader_enter_struct(&r) && reader_skip(&r) && reader_string(&r, &names[i], nullptr) && reader_leave(&r);
    ok = ok && !reader_more(r) && reader_leave(&r) && text_equal(names[0], "first") && text_equal(names[1], "second");
    check(ok, "variants skipped inside structs inside an array");
    builder_release(&b);
}

void test_hostile_bodies() noexcept {
    // A string that runs past the end, lacks its NUL, or holds one inside.
    {
        const u8 truncated[] = {10, 0, 0, 0, 'a', 'b'};
        Reader r;
        reader_init(&r, truncated, sizeof(truncated), "s", nullptr, 0);
        const char* s;
        check(!reader_string(&r, &s, nullptr), "a string longer than the body");
        const u8 unterminated[] = {2, 0, 0, 0, 'a', 'b', 'c'};
        reader_init(&r, unterminated, sizeof(unterminated), "s", nullptr, 0);
        check(!reader_string(&r, &s, nullptr), "a string without its terminator");
        const u8 inner[] = {3, 0, 0, 0, 'a', 0, 'b', 0};
        reader_init(&r, inner, sizeof(inner), "s", nullptr, 0);
        check(!reader_string(&r, &s, nullptr), "a string holding a NUL");
        const u8 badbool[] = {2, 0, 0, 0};
        reader_init(&r, badbool, sizeof(badbool), "b", nullptr, 0);
        bool v;
        check(!reader_bool(&r, &v), "a boolean other than 0 and 1");
    }
    // An array whose length passes the body, or whose elements overrun the declared length.
    {
        const u8 overlong[] = {200, 0, 0, 0, 1, 2};
        Reader r;
        reader_init(&r, overlong, sizeof(overlong), "ay", nullptr, 0);
        check(!reader_enter_array(&r), "an array longer than the body");
        const u8 short_array[] = {2, 0, 0, 0, 1, 0, 0, 0};
        reader_init(&r, short_array, sizeof(short_array), "au", nullptr, 0);
        check(reader_enter_array(&r) && reader_more(r), "an array whose length is not a whole number of elements enters");
        u32 value;
        check(!reader_u32(&r, &value), "but its element cannot be read past the declared end");
    }
    // Variants with bad signatures.
    {
        const u8 bad[] = {2, 'a', 0, 0};
        Reader r;
        reader_init(&r, bad, sizeof(bad), "v", nullptr, 0);
        check(!reader_enter_variant(&r, nullptr), "a variant whose signature is not a complete type");
        const u8 unterminated[] = {1, 'u', 5, 0, 0, 0};
        reader_init(&r, unterminated, sizeof(unterminated), "v", nullptr, 0);
        check(!reader_enter_variant(&r, nullptr), "a variant signature without its terminator");
    }
    // File descriptors: an index past what the message carries.
    {
        const u8 body[] = {3, 0, 0, 0};
        const int fds[1] = {5};
        Reader r;
        reader_init(&r, body, sizeof(body), "h", fds, 1);
        int fd;
        check(!reader_unix_fd(&r, &fd), "a descriptor index past the carried ones");
        const u8 first[] = {0, 0, 0, 0};
        reader_init(&r, first, sizeof(first), "h", fds, 1);
        check(reader_unix_fd(&r, &fd) && fd == 5, "a descriptor index within them");
    }
    // Deeply nested variants and arrays stop at the depth limit.
    {
        u8 deep[96];
        for (u32 i = 0; i < 32; ++i) {
            deep[i * 3] = 1;
            deep[i * 3 + 1] = 'v';
            deep[i * 3 + 2] = 0;
        }
        Reader r;
        char sig[3] = {'v', 0, 0};
        u32 depth = 0;
        reader_init(&r, deep, sizeof(deep), sig, nullptr, 0);
        while (reader_enter_variant(&r, nullptr))
            ++depth;
        check(depth <= kMaxDepth && r.failed, "variant nesting stops at the depth limit");
    }
}

void fill_header(MessageHeader* header, MessageType type) noexcept {
    clear_bytes(header, static_cast<u32>(sizeof(*header)));
    header->type = type;
    header->serial = 7;
}

void test_messages() noexcept {
    Builder body;
    builder_init(&body);
    builder_string(&body, "arg");
    builder_u32(&body, 42);
    MessageHeader header;
    fill_header(&header, MessageType::method_call);
    header.path = "/org/test";
    header.interface = "org.test.Iface";
    header.member = "Method";
    header.destination = "org.test.Service";
    u8* block = nullptr;
    u32 bytes = 0;
    check(message_serialize(header, body, &block, &bytes), "a method call serialises");
    check(message_size_at(block, 15) == 0 && message_size_at(block, 16) == bytes && message_size_at(block, bytes) == bytes,
          "the size comes from the first 16 bytes");

    Message message;
    check(message_parse(&message, block, bytes), "and parses");
    check(message.type == MessageType::method_call && message.serial == 7 && message.flags == 0 &&
              text_equal(message.path, "/org/test") && text_equal(message.interface, "org.test.Iface") &&
              text_equal(message.member, "Method") && text_equal(message.destination, "org.test.Service") &&
              text_equal(message.signature, "su") && message.sender == nullptr && message.body_size == body.size &&
              message.body_offset % 8 == 0,
          "header fields survive");
    Reader r;
    message_reader(message, &r);
    const char* text = nullptr;
    u32 number = 0;
    check(reader_string(&r, &text, nullptr) && text_equal(text, "arg") && reader_u32(&r, &number) && number == 42,
          "the body reads from the message");
    Message clone;
    check(message_clone(&clone, message) && text_equal(clone.member, "Method") && clone.buffer != message.buffer &&
              clone.body_size == message.body_size,
          "a clone owns its block");
    message_release(&message);
    check(clone.buffer != nullptr, "releasing the original keeps the clone");
    message_release(&clone);
    builder_release(&body);

    // The other types, and what each requires.
    Builder none;
    builder_init(&none);
    fill_header(&header, MessageType::method_return);
    check(!message_serialize(header, none, &block, &bytes), "a method return needs a reply serial");
    header.reply_serial = 3;
    header.destination = ":1.5";
    check(message_serialize(header, none, &block, &bytes) && message_parse(&message, block, bytes) &&
              message.type == MessageType::method_return && message.reply_serial == 3 && text_equal(message.signature, "") &&
              message.body_size == 0 && text_equal(message.destination, ":1.5"),
          "a method return with an empty body");
    message_release(&message);

    fill_header(&header, MessageType::error);
    header.reply_serial = 9;
    header.error_name = "org.test.Error.Bad";
    check(message_serialize(header, none, &block, &bytes) && message_parse(&message, block, bytes) &&
              message.type == MessageType::error && text_equal(message.error_name, "org.test.Error.Bad"),
          "an error");
    message_release(&message);

    fill_header(&header, MessageType::signal);
    header.path = "/p";
    header.interface = "a.b";
    header.member = "M";
    header.flags = flag_no_reply_expected;
    check(message_serialize(header, none, &block, &bytes) && message_parse(&message, block, bytes) &&
              message.type == MessageType::signal && message.flags == flag_no_reply_expected,
          "a signal");
    message_release(&message);
    header.interface = nullptr;
    check(!message_serialize(header, none, &block, &bytes), "a signal needs an interface");
    fill_header(&header, MessageType::method_call);
    check(!message_serialize(header, none, &block, &bytes), "a call needs a path and a member");
    builder_release(&none);
}

// A deterministic generator for the mutation test.
u32 g_seed = 0x2468ACE1;
u32 next_random() noexcept {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

void test_mutations() noexcept {
    Builder body;
    builder_init(&body);
    write_everything(&body);
    MessageHeader header;
    fill_header(&header, MessageType::signal);
    header.path = "/p";
    header.interface = "a.b";
    header.member = "M";
    u8* original = nullptr;
    u32 bytes = 0;
    check(message_serialize(header, body, &original, &bytes), "the mutation subject serialises");
    u32 parsed = 0;
    u32 walked = 0;
    for (u32 round = 0; round < 40000; ++round) {
        u8* copy = static_cast<u8*>(heap_allocate(bytes + 1));
        copy_bytes(copy, original, bytes);
        u32 size = bytes;
        const u32 flips = 1 + next_random() % 4;
        for (u32 i = 0; i < flips; ++i)
            copy[next_random() % bytes] = static_cast<u8>(next_random());
        if (round % 7 == 0)
            size = next_random() % (bytes + 1);  // truncated: the block is shorter than the header says
        Message message;
        if (!message_parse(&message, copy, size))
            continue;
        ++parsed;
        Reader r;
        message_reader(message, &r);
        u32 guard = 0;
        while (reader_more(r) && reader_skip(&r) && ++guard < 100)
            ;
        walked += r.failed ? 0 : 1;
        message_release(&message);
    }
    heap_free(original);
    builder_release(&body);
    check(parsed > 0 && walked > 0, "mutated messages are parsed or refused, never crashing or looping");
}

bool address_is(const char* address, u32 index, const char* path, bool abstract_name) noexcept {
    SocketAddress entries[4];
    const u32 count = address_parse(address, entries, 4);
    if (index >= count)
        return false;
    const SocketAddress& e = entries[index];
    u32 length = 0;
    while (path[length])
        ++length;
    if (e.data[0] != 1)
        return false;
    if (abstract_name)
        return e.length == 3 + length && e.data[2] == 0 && bytes_equal(e.data + 3, path, length);
    return e.length == 2 + length + 1 && bytes_equal(e.data + 2, path, length + 1);
}

void test_addresses() noexcept {
    check(address_is("unix:path=/run/user/1000/bus", 0, "/run/user/1000/bus", false), "unix:path");
    check(address_is("unix:abstract=/tmp/dbus-abc,guid=0123", 0, "/tmp/dbus-abc", true), "unix:abstract with a guid");
    check(address_is("unix:guid=ff,path=/a/b", 0, "/a/b", false), "keys in any order");
    check(address_is("unix:path=/a%20b%2fc", 0, "/a b/c", false), "percent escapes");
    SocketAddress entries[4];
    check(address_parse("tcp:host=x,port=1;unix:path=/one;systemd:;unix:abstract=two", entries, 4) == 2 &&
              address_is("tcp:host=x,port=1;unix:path=/one;systemd:;unix:abstract=two", 0, "/one", false) &&
              address_is("tcp:host=x,port=1;unix:path=/one;systemd:;unix:abstract=two", 1, "two", true),
          "several entries, other transports skipped");
    check(address_parse("", entries, 4) == 0 && address_parse("unix:", entries, 4) == 0 &&
              address_parse("unix:path=", entries, 4) == 0 && address_parse("unix:path=%zz", entries, 4) == 0 &&
              address_parse("unix:path=%4", entries, 4) == 0 && address_parse("garbage", entries, 4) == 0,
          "malformed addresses give none");
    char longpath[200] = "unix:path=/";
    u32 length = 11;
    while (length < 190)
        longpath[length++] = 'x';
    longpath[length] = 0;
    check(address_parse(longpath, entries, 4) == 0, "a path too long for sockaddr_un is refused");
    check(address_parse("unix:path=/a;unix:path=/b", entries, 1) == 1, "the capacity bounds the result");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_dbus_wire_entry) {
    maicord::test_vectors();
    maicord::test_builder_errors();
    maicord::test_round_trip();
    maicord::test_variant_skip();
    maicord::test_hostile_bodies();
    maicord::test_messages();
    maicord::test_mutations();
    maicord::test_addresses();
    maicord::test_exit(maicord::g_failures, "dbus_wire");
}
