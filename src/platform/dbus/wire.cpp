#include "maicord/core/heap.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/platform/dbus/wire.hpp"
#include "maicord/sys/linux/calls.hpp"

namespace maicord::dbus {
namespace {

constexpr u32 kMaxArrayBytes = 64u << 20;  // the specification's limit of an array

u32 alignment_of(char type) noexcept {
    switch (type) {
    case 'y':
    case 'g':
    case 'v':
        return 1;
    case 'n':
    case 'q':
        return 2;
    case 'x':
    case 't':
    case 'd':
    case '(':
    case '{':
        return 8;
    default:
        return 4;  // b i u s o a h
    }
}

u32 string_length(const char* text) noexcept {
    u32 length = 0;
    while (text[length])
        ++length;
    return length;
}

// Signatures ---------------------------------------------------------------------------------------------------------

u32 type_length(const char* s, u32 depth) noexcept {
    if (depth > 32)
        return 0;
    switch (s[0]) {
    case 'y': case 'b': case 'n': case 'q': case 'i': case 'u': case 'x': case 't': case 'd': case 's': case 'o':
    case 'g': case 'v': case 'h':
        return 1;
    case 'a': {
        const u32 element = type_length(s + 1, depth + 1);
        return element ? 1 + element : 0;
    }
    case '(': {
        u32 length = 1;
        u32 members = 0;
        while (s[length] != ')') {
            const u32 member = type_length(s + length, depth + 1);
            if (!member)
                return 0;
            length += member;
            ++members;
        }
        return members ? length + 1 : 0;
    }
    case '{': {
        // A dictionary entry: a basic key (not a variant) and one value.
        const char key = s[1];
        if (!(key == 'y' || key == 'b' || key == 'n' || key == 'q' || key == 'i' || key == 'u' || key == 'x' ||
              key == 't' || key == 'd' || key == 's' || key == 'o' || key == 'g' || key == 'h'))
            return 0;
        const u32 value = type_length(s + 2, depth + 1);
        return value && s[2 + value] == '}' ? 3 + value : 0;
    }
    default:
        return 0;
    }
}

// Builder ------------------------------------------------------------------------------------------------------------

bool ensure(Builder* b, u32 extra) noexcept {
    if (b->failed)
        return false;
    if (extra > kMaxMessageBytes || b->size > kMaxMessageBytes - extra) {
        b->failed = true;
        return false;
    }
    const u32 needed = b->size + extra;
    if (needed <= b->capacity)
        return true;
    u32 capacity = b->capacity ? b->capacity : 256;
    while (capacity < needed)
        capacity *= 2;
    u8* grown = static_cast<u8*>(heap_allocate(capacity));
    if (!grown) {
        b->failed = true;
        return false;
    }
    if (b->size)
        copy_bytes(grown, b->data, b->size);
    if (b->data) {
        secure_clear_bytes(b->data, b->size);
        heap_free(b->data);
    }
    b->data = grown;
    b->capacity = capacity;
    return true;
}

void pad_to(Builder* b, u32 alignment) noexcept {
    const u32 pad = (alignment - (b->size & (alignment - 1))) & (alignment - 1);
    if (!pad || !ensure(b, pad))
        return;
    for (u32 i = 0; i < pad; ++i)
        b->data[b->size++] = 0;
}

void put(Builder* b, const void* value, u32 bytes, u32 alignment) noexcept {
    pad_to(b, alignment);
    if (!ensure(b, bytes))
        return;
    copy_bytes(b->data + b->size, value, bytes);
    b->size += bytes;
}

// Records `text` in the top-level signature when no container is open.
void note(Builder* b, const char* text, u32 length) noexcept {
    if (b->depth != 0 || b->failed)
        return;
    if (b->signature_length + length > kMaxSignature) {
        b->failed = true;
        return;
    }
    copy_bytes(b->signature + b->signature_length, text, length);
    b->signature_length += length;
    b->signature[b->signature_length] = 0;
}

bool push(Builder* b, u8 kind, u32 length_position, u32 data_start) noexcept {
    if (b->failed || b->depth >= kMaxDepth) {
        b->failed = true;
        return false;
    }
    b->frames[b->depth++] = Builder::Frame{kind, length_position, data_start};
    return true;
}

bool pop(Builder* b, u8 kind) noexcept {
    if (b->failed || b->depth == 0 || b->frames[b->depth - 1].kind != kind) {
        b->failed = true;
        return false;
    }
    --b->depth;
    return true;
}

// Reader -------------------------------------------------------------------------------------------------------------

bool reader_fail(Reader* r) noexcept {
    r->failed = true;
    return false;
}

// The end of the innermost array, or of the body.
u32 limit_of(const Reader& r) noexcept {
    for (u32 i = r.depth; i-- > 0;)
        if (r.frames[i].kind == 'a')
            return r.frames[i].end;
    return r.size;
}

bool align_reader(Reader* r, u32 alignment) noexcept {
    const u32 pad = (alignment - (r->position & (alignment - 1))) & (alignment - 1);
    if (r->position > limit_of(*r) || pad > limit_of(*r) - r->position)
        return reader_fail(r);
    r->position += pad;
    return true;
}

// After one complete value was consumed at the current level: the signature moves past it.
void finished(Reader* r, u32 type_chars) noexcept {
    if (r->depth > 0 && r->frames[r->depth - 1].kind == 'a')
        r->signature_position = r->frames[r->depth - 1].element_start;
    else
        r->signature_position += type_chars;
}

bool take(Reader* r, char type, void* value, u32 bytes) noexcept {
    if (r->failed || reader_type(*r) != type)
        return reader_fail(r);
    if (!align_reader(r, alignment_of(type)))
        return false;
    if (bytes > limit_of(*r) - r->position)
        return reader_fail(r);
    copy_bytes(value, r->data + r->position, bytes);
    r->position += bytes;
    finished(r, 1);
    return true;
}

bool push_frame(Reader* r, const Reader::Frame& frame) noexcept {
    if (r->depth >= kMaxDepth)
        return reader_fail(r);
    r->frames[r->depth++] = frame;
    return true;
}

} // namespace

u32 signature_type_length(const char* signature) noexcept {
    return signature ? type_length(signature, 0) : 0;
}

bool signature_valid(const char* signature) noexcept {
    if (!signature || string_length(signature) > kMaxSignature)
        return false;
    while (*signature) {
        const u32 length = type_length(signature, 0);
        if (!length)
            return false;
        signature += length;
    }
    return true;
}

// Builder ------------------------------------------------------------------------------------------------------------

void builder_init(Builder* builder) noexcept {
    clear_bytes(builder, static_cast<u32>(sizeof(*builder)));
}

void builder_release(Builder* builder) noexcept {
    for (u32 i = 0; i < builder->fd_count; ++i)
        sys_close(builder->fds[i]);
    if (builder->data) {
        secure_clear_bytes(builder->data, builder->capacity);
        heap_free(builder->data);
    }
    clear_bytes(builder, static_cast<u32>(sizeof(*builder)));
}

void builder_byte(Builder* builder, u8 value) noexcept {
    note(builder, "y", 1);
    put(builder, &value, 1, 1);
}

void builder_bool(Builder* builder, bool value) noexcept {
    const u32 word = value ? 1 : 0;
    note(builder, "b", 1);
    put(builder, &word, 4, 4);
}

void builder_i16(Builder* builder, i16 value) noexcept {
    note(builder, "n", 1);
    put(builder, &value, 2, 2);
}

void builder_u16(Builder* builder, u16 value) noexcept {
    note(builder, "q", 1);
    put(builder, &value, 2, 2);
}

void builder_i32(Builder* builder, i32 value) noexcept {
    note(builder, "i", 1);
    put(builder, &value, 4, 4);
}

void builder_u32(Builder* builder, u32 value) noexcept {
    note(builder, "u", 1);
    put(builder, &value, 4, 4);
}

void builder_i64(Builder* builder, i64 value) noexcept {
    note(builder, "x", 1);
    put(builder, &value, 8, 8);
}

void builder_u64(Builder* builder, u64 value) noexcept {
    note(builder, "t", 1);
    put(builder, &value, 8, 8);
}

void builder_double(Builder* builder, double value) noexcept {
    note(builder, "d", 1);
    put(builder, &value, 8, 8);
}

static void put_string(Builder* builder, const char* text, u32 length, bool is_path) noexcept {
    note(builder, is_path ? "o" : "s", 1);
    // A NUL inside a string cannot be represented: the receiver would see a shorter one.
    for (u32 i = 0; i < length; ++i) {
        if (text[i] == 0) {
            builder->failed = true;
            return;
        }
    }
    const u32 word = length;
    put(builder, &word, 4, 4);
    if (!ensure(builder, length + 1))
        return;
    if (length)
        copy_bytes(builder->data + builder->size, text, length);
    builder->data[builder->size + length] = 0;
    builder->size += length + 1;
}

void builder_string(Builder* builder, const char* text, u32 length) noexcept {
    put_string(builder, text, length, false);
}

void builder_string(Builder* builder, const char* text) noexcept {
    put_string(builder, text, string_length(text), false);
}

void builder_object_path(Builder* builder, const char* path) noexcept {
    put_string(builder, path, string_length(path), true);
}

void builder_signature(Builder* builder, const char* signature) noexcept {
    note(builder, "g", 1);
    const u32 length = string_length(signature);
    if (!signature_valid(signature)) {
        builder->failed = true;
        return;
    }
    const u8 byte = static_cast<u8>(length);
    put(builder, &byte, 1, 1);
    if (!ensure(builder, length + 1))
        return;
    copy_bytes(builder->data + builder->size, signature, length + 1);
    builder->size += length + 1;
}

void builder_unix_fd(Builder* builder, int fd) noexcept {
    note(builder, "h", 1);
    if (builder->failed)
        return;
    if (builder->fd_count >= kMaxFds) {
        builder->failed = true;
        return;
    }
    // F_DUPFD_CLOEXEC = 1030
    const long duplicate = syscall3(72 /* fcntl */, fd, 1030, 0);
    if (syscall_failed(duplicate)) {
        builder->failed = true;
        return;
    }
    const u32 index = builder->fd_count;
    builder->fds[builder->fd_count++] = static_cast<int>(duplicate);
    put(builder, &index, 4, 4);
}

void builder_open_array(Builder* builder, const char* element_signature) noexcept {
    const u32 length = string_length(element_signature);
    if (type_length(element_signature, 0) != length) {
        builder->failed = true;
        return;
    }
    if (builder->depth == 0) {
        note(builder, "a", 1);
        note(builder, element_signature, length);
    }
    pad_to(builder, 4);
    const u32 length_position = builder->size;
    const u32 zero = 0;
    put(builder, &zero, 4, 4);
    pad_to(builder, alignment_of(element_signature[0]));
    push(builder, 'a', length_position, builder->size);
}

void builder_close_array(Builder* builder) noexcept {
    if (!pop(builder, 'a'))
        return;
    const Builder::Frame& frame = builder->frames[builder->depth];
    const u32 length = builder->size - frame.data_start;
    if (length > kMaxArrayBytes) {
        builder->failed = true;
        return;
    }
    copy_bytes(builder->data + frame.length_position, &length, 4);
}

void builder_open_struct(Builder* builder, const char* full_signature) noexcept {
    const u32 length = string_length(full_signature);
    if (full_signature[0] != '(' || type_length(full_signature, 0) != length) {
        builder->failed = true;
        return;
    }
    note(builder, full_signature, length);
    pad_to(builder, 8);
    push(builder, '(', 0, 0);
}

void builder_close_struct(Builder* builder) noexcept {
    pop(builder, '(');
}

void builder_open_entry(Builder* builder) noexcept {
    pad_to(builder, 8);
    push(builder, '{', 0, 0);
}

void builder_close_entry(Builder* builder) noexcept {
    pop(builder, '{');
}

void builder_open_variant(Builder* builder, const char* value_signature) noexcept {
    const u32 length = string_length(value_signature);
    if (type_length(value_signature, 0) != length) {
        builder->failed = true;
        return;
    }
    note(builder, "v", 1);
    const u8 byte = static_cast<u8>(length);
    put(builder, &byte, 1, 1);
    if (ensure(builder, length + 1)) {
        copy_bytes(builder->data + builder->size, value_signature, length + 1);
        builder->size += length + 1;
    }
    push(builder, 'v', 0, 0);
}

void builder_close_variant(Builder* builder) noexcept {
    pop(builder, 'v');
}

void builder_byte_array(Builder* builder, const u8* bytes, u32 count) noexcept {
    builder_open_array(builder, "y");
    if (count && ensure(builder, count)) {
        copy_bytes(builder->data + builder->size, bytes, count);
        builder->size += count;
    }
    builder_close_array(builder);
}

// Reader -------------------------------------------------------------------------------------------------------------

void reader_init(Reader* reader, const u8* body, u32 size, const char* signature, const int* fds, u32 fd_count) noexcept {
    clear_bytes(reader, static_cast<u32>(sizeof(*reader)));
    reader->data = body;
    reader->size = size;
    reader->signature = signature ? signature : "";
    reader->fds = fds;
    reader->fd_count = fd_count;
}

char reader_type(const Reader& reader) noexcept {
    if (reader.failed)
        return 0;
    if (reader.depth > 0 && reader.frames[reader.depth - 1].kind == 'a') {
        const Reader::Frame& frame = reader.frames[reader.depth - 1];
        return reader.position >= frame.end ? 0 : reader.signature[frame.element_start];
    }
    const char type = reader.signature[reader.signature_position];
    return type == ')' || type == '}' ? 0 : type;
}

bool reader_byte(Reader* reader, u8* value) noexcept {
    return take(reader, 'y', value, 1);
}

bool reader_bool(Reader* reader, bool* value) noexcept {
    u32 word = 0;
    if (!take(reader, 'b', &word, 4))
        return false;
    if (word > 1)
        return reader_fail(reader);
    *value = word == 1;
    return true;
}

bool reader_i16(Reader* reader, i16* value) noexcept {
    return take(reader, 'n', value, 2);
}

bool reader_u16(Reader* reader, u16* value) noexcept {
    return take(reader, 'q', value, 2);
}

bool reader_i32(Reader* reader, i32* value) noexcept {
    return take(reader, 'i', value, 4);
}

bool reader_u32(Reader* reader, u32* value) noexcept {
    return take(reader, 'u', value, 4);
}

bool reader_i64(Reader* reader, i64* value) noexcept {
    return take(reader, 'x', value, 8);
}

bool reader_u64(Reader* reader, u64* value) noexcept {
    return take(reader, 't', value, 8);
}

bool reader_double(Reader* reader, double* value) noexcept {
    return take(reader, 'd', value, 8);
}

bool reader_string(Reader* reader, const char** text, u32* length) noexcept {
    if (reader->failed)
        return false;
    const char type = reader_type(*reader);
    if (type != 's' && type != 'o' && type != 'g')
        return reader_fail(reader);
    u32 bytes = 0;
    if (type == 'g') {
        u8 small = 0;
        if (!align_reader(reader, 1) || 1 > limit_of(*reader) - reader->position)
            return reader_fail(reader);
        small = reader->data[reader->position++];
        bytes = small;
    } else {
        if (!align_reader(reader, 4) || 4 > limit_of(*reader) - reader->position)
            return reader_fail(reader);
        copy_bytes(&bytes, reader->data + reader->position, 4);
        reader->position += 4;
    }
    const u32 room = limit_of(*reader) - reader->position;
    if (bytes >= room || reader->data[reader->position + bytes] != 0)
        return reader_fail(reader);
    for (u32 i = 0; i < bytes; ++i)
        if (reader->data[reader->position + i] == 0)
            return reader_fail(reader);
    *text = reinterpret_cast<const char*>(reader->data + reader->position);
    if (length)
        *length = bytes;
    reader->position += bytes + 1;
    finished(reader, 1);
    return true;
}

bool reader_unix_fd(Reader* reader, int* fd) noexcept {
    u32 index = 0;
    if (!take(reader, 'h', &index, 4))
        return false;
    if (index >= reader->fd_count)
        return reader_fail(reader);
    if (reader->fds[index] < 0)
        return reader_fail(reader);
    *fd = reader->fds[index];
    return true;
}

bool reader_enter_array(Reader* reader) noexcept {
    if (reader->failed || reader_type(*reader) != 'a')
        return reader_fail(reader);
    const u32 position_in_signature = reader->signature_position;
    u32 length = 0;
    if (!align_reader(reader, 4) || 4 > limit_of(*reader) - reader->position)
        return reader_fail(reader);
    copy_bytes(&length, reader->data + reader->position, 4);
    reader->position += 4;
    const char element = reader->signature[position_in_signature + 1];
    if (length > kMaxArrayBytes || !align_reader(reader, alignment_of(element)) ||
        length > limit_of(*reader) - reader->position)
        return reader_fail(reader);
    Reader::Frame frame{};
    frame.kind = 'a';
    frame.end = reader->position + length;
    frame.saved_signature = reader->signature;
    frame.saved_position = position_in_signature + type_length(reader->signature + position_in_signature, 0);
    frame.element_start = position_in_signature + 1;
    if (!push_frame(reader, frame))
        return false;
    reader->signature_position = frame.element_start;
    return true;
}

bool reader_enter_struct(Reader* reader) noexcept {
    const char type = reader_type(*reader);
    if (reader->failed || (type != '(' && type != '{'))
        return reader_fail(reader);
    if (!align_reader(reader, 8))
        return false;
    Reader::Frame frame{};
    frame.kind = '(';
    frame.saved_signature = reader->signature;
    frame.saved_position = reader->signature_position + type_length(reader->signature + reader->signature_position, 0);
    if (!push_frame(reader, frame))
        return false;
    ++reader->signature_position;
    return true;
}

bool reader_enter_variant(Reader* reader, const char** value_signature) noexcept {
    if (reader->failed || reader_type(*reader) != 'v')
        return reader_fail(reader);
    const char* signature = nullptr;
    u32 length = 0;
    // The variant's own signature is a 'g' at this position: read it without moving the outer signature.
    if (reader->position >= limit_of(*reader))
        return reader_fail(reader);
    length = reader->data[reader->position];
    const u32 room = limit_of(*reader) - reader->position - 1;
    if (length >= room || reader->data[reader->position + 1 + length] != 0)
        return reader_fail(reader);
    signature = reinterpret_cast<const char*>(reader->data + reader->position + 1);
    if (type_length(signature, 0) != length)
        return reader_fail(reader);
    reader->position += length + 2;
    Reader::Frame frame{};
    frame.kind = 'v';
    frame.saved_signature = reader->signature;
    frame.saved_position = reader->signature_position + 1;
    if (!push_frame(reader, frame))
        return false;
    reader->signature = signature;
    reader->signature_position = 0;
    if (value_signature)
        *value_signature = signature;
    return true;
}

bool reader_leave(Reader* reader) noexcept {
    if (reader->failed || reader->depth == 0)
        return reader_fail(reader);
    Reader::Frame& frame = reader->frames[reader->depth - 1];
    if (frame.kind == 'a') {
        if (reader->position > frame.end)
            return reader_fail(reader);
        reader->position = frame.end;
    } else {
        while (reader_more(*reader))
            if (!reader_skip(reader))
                return false;
    }
    reader->signature = frame.saved_signature;
    reader->signature_position = frame.saved_position;
    --reader->depth;
    if (reader->depth > 0 && reader->frames[reader->depth - 1].kind == 'a')
        reader->signature_position = reader->frames[reader->depth - 1].element_start;
    return true;
}

bool reader_skip(Reader* reader) noexcept {
    switch (reader_type(*reader)) {
    case 'y': {
        u8 value;
        return reader_byte(reader, &value);
    }
    case 'b': {
        bool value;
        return reader_bool(reader, &value);
    }
    case 'n': {
        i16 value;
        return reader_i16(reader, &value);
    }
    case 'q': {
        u16 value;
        return reader_u16(reader, &value);
    }
    case 'i': {
        i32 value;
        return reader_i32(reader, &value);
    }
    case 'u': {
        u32 value;
        return reader_u32(reader, &value);
    }
    case 'h': {
        u32 value;
        return take(reader, 'h', &value, 4);
    }
    case 'x': {
        i64 value;
        return reader_i64(reader, &value);
    }
    case 't': {
        u64 value;
        return reader_u64(reader, &value);
    }
    case 'd': {
        double value;
        return reader_double(reader, &value);
    }
    case 's':
    case 'o':
    case 'g': {
        const char* text;
        return reader_string(reader, &text, nullptr);
    }
    case 'a':
        if (!reader_enter_array(reader))
            return false;
        return reader_leave(reader);
    case '(':
    case '{':
        if (!reader_enter_struct(reader))
            return false;
        return reader_leave(reader);
    case 'v':
        if (!reader_enter_variant(reader, nullptr))
            return false;
        return reader_leave(reader);
    default:
        return reader_fail(reader);
    }
}

bool reader_byte_array(Reader* reader, const u8** bytes, u32* count) noexcept {
    if (reader->failed || reader_type(*reader) != 'a' || reader->signature[reader->signature_position + 1] != 'y')
        return reader_fail(reader);
    if (!reader_enter_array(reader))
        return false;
    const Reader::Frame& frame = reader->frames[reader->depth - 1];
    *bytes = reader->data + reader->position;
    *count = frame.end - reader->position;
    reader->position = frame.end;
    return reader_leave(reader);
}

// Messages -----------------------------------------------------------------------------------------------------------

namespace {

void field_string(Builder* b, u8 code, char type, const char* value) noexcept {
    builder_open_struct(b, "(yv)");
    builder_byte(b, code);
    const char sig[2] = {type, 0};
    builder_open_variant(b, sig);
    if (type == 'g')
        builder_signature(b, value);
    else if (type == 'o')
        builder_object_path(b, value);
    else
        builder_string(b, value);
    builder_close_variant(b);
    builder_close_struct(b);
}

void field_u32(Builder* b, u8 code, u32 value) noexcept {
    builder_open_struct(b, "(yv)");
    builder_byte(b, code);
    builder_open_variant(b, "u");
    builder_u32(b, value);
    builder_close_variant(b);
    builder_close_struct(b);
}

} // namespace

bool message_serialize(const MessageHeader& header, const Builder& body, u8** block, u32* bytes) noexcept {
    if (body.failed || body.depth != 0 || header.type == MessageType::invalid)
        return false;
    switch (header.type) {
    case MessageType::method_call:
        if (!header.path || !header.member)
            return false;
        break;
    case MessageType::signal:
        if (!header.path || !header.interface || !header.member)
            return false;
        break;
    case MessageType::error:
        if (!header.error_name)
            return false;
        [[fallthrough]];
    case MessageType::method_return:
        if (header.reply_serial == 0)
            return false;
        break;
    default:
        return false;
    }
    Builder h;
    builder_init(&h);
    // The fixed part: byte order, type, flags, version, body length (patched), serial; then the field array.
    builder_byte(&h, 'l');
    builder_byte(&h, static_cast<u8>(header.type));
    builder_byte(&h, header.flags);
    builder_byte(&h, 1);
    builder_u32(&h, body.size);
    builder_u32(&h, header.serial);
    builder_open_array(&h, "(yv)");
    if (header.path)
        field_string(&h, 1, 'o', header.path);
    if (header.interface)
        field_string(&h, 2, 's', header.interface);
    if (header.member)
        field_string(&h, 3, 's', header.member);
    if (header.error_name)
        field_string(&h, 4, 's', header.error_name);
    if (header.type == MessageType::method_return || header.type == MessageType::error)
        field_u32(&h, 5, header.reply_serial);
    if (header.destination)
        field_string(&h, 6, 's', header.destination);
    if (body.signature_length)
        field_string(&h, 8, 'g', body.signature);
    if (body.fd_count)
        field_u32(&h, 9, body.fd_count);
    builder_close_array(&h);
    pad_to(&h, 8);
    const u32 header_bytes = h.size;
    if (h.failed || header_bytes > kMaxMessageBytes || body.size > kMaxMessageBytes - header_bytes) {
        builder_release(&h);
        return false;
    }
    u8* out = static_cast<u8*>(heap_allocate(header_bytes + body.size + 1));
    if (!out) {
        builder_release(&h);
        return false;
    }
    copy_bytes(out, h.data, header_bytes);
    if (body.size)
        copy_bytes(out + header_bytes, body.data, body.size);
    builder_release(&h);
    *block = out;
    *bytes = header_bytes + body.size;
    return true;
}

u32 message_size_at(const u8* bytes, u32 count) noexcept {
    if (count < 16)
        return 0;
    if (bytes[0] != 'l' || bytes[3] != 1)
        return kMaxMessageBytes + 1;
    u32 body = 0;
    u32 fields = 0;
    copy_bytes(&body, bytes + 4, 4);
    copy_bytes(&fields, bytes + 12, 4);
    const u64 header = (16ull + fields + 7) & ~7ull;
    const u64 total = header + body;
    if (fields > kMaxArrayBytes || total > kMaxMessageBytes)
        return kMaxMessageBytes + 1;
    return static_cast<u32>(total);
}

bool message_parse(Message* message, u8* buffer, u32 size) noexcept {
    clear_bytes(message, static_cast<u32>(sizeof(*message)));
    message->buffer = buffer;
    message->buffer_size = size;
    for (u32 i = 0; i < kMaxFds; ++i)
        message->fds[i] = -1;
    const auto fail = [&]() {
        message_release(message);
        return false;
    };
    if (message_size_at(buffer, size) != size)
        return fail();
    message->type = static_cast<MessageType>(buffer[1]);
    message->flags = buffer[2];
    u32 body = 0;
    u32 fields = 0;
    copy_bytes(&body, buffer + 4, 4);
    copy_bytes(&message->serial, buffer + 8, 4);
    copy_bytes(&fields, buffer + 12, 4);
    if (message->type < MessageType::method_call || message->type > MessageType::signal || message->serial == 0)
        return fail();
    const u32 header = static_cast<u32>((16ull + fields + 7) & ~7ull);
    message->body_offset = header;
    message->body_size = body;
    message->signature = "";
    u32 position = 16;
    const u32 end = 16 + fields;
    u32 declared_fds = 0;
    while (position < end) {
        position = (position + 7) & ~7u;
        if (position >= end || end - position < 4)
            return fail();
        const u8 code = buffer[position++];
        const u8 signature_length = buffer[position++];
        if (signature_length != 1 || position + 2 > end || buffer[position + 1] != 0)
            return fail();
        const char type = static_cast<char>(buffer[position]);
        position += 2;
        const char* text = nullptr;
        u32 value = 0;
        if (type == 'o' || type == 's') {
            position = (position + 3) & ~3u;
            u32 length = 0;
            if (position + 4 > end)
                return fail();
            copy_bytes(&length, buffer + position, 4);
            position += 4;
            if (length >= end - position || buffer[position + length] != 0)
                return fail();
            text = reinterpret_cast<const char*>(buffer + position);
            position += length + 1;
        } else if (type == 'g') {
            if (position >= end)
                return fail();
            const u32 length = buffer[position++];
            if (length >= end - position || buffer[position + length] != 0)
                return fail();
            text = reinterpret_cast<const char*>(buffer + position);
            position += length + 1;
        } else if (type == 'u') {
            position = (position + 3) & ~3u;
            if (position + 4 > end)
                return fail();
            copy_bytes(&value, buffer + position, 4);
            position += 4;
        } else {
            return fail();  // no field has another type
        }
        switch (code) {
        case 1: message->path = (type == 'o') ? text : nullptr; break;
        case 2: message->interface = (type == 's') ? text : nullptr; break;
        case 3: message->member = (type == 's') ? text : nullptr; break;
        case 4: message->error_name = (type == 's') ? text : nullptr; break;
        case 5: message->reply_serial = (type == 'u') ? value : 0; break;
        case 6: message->destination = (type == 's') ? text : nullptr; break;
        case 7: message->sender = (type == 's') ? text : nullptr; break;
        case 8: message->signature = (type == 'g') ? text : nullptr; break;
        case 9: declared_fds = (type == 'u') ? value : 0; break;
        default: break;  // unknown fields are ignored
        }
    }
    if (!message->signature || !signature_valid(message->signature) || declared_fds > kMaxFds)
        return fail();
    message->fd_count = declared_fds;  // the connection fills the descriptors in
    switch (message->type) {
    case MessageType::method_call:
        if (!message->path || !message->member)
            return fail();
        break;
    case MessageType::signal:
        if (!message->path || !message->interface || !message->member)
            return fail();
        break;
    case MessageType::error:
        if (!message->error_name)
            return fail();
        [[fallthrough]];
    case MessageType::method_return:
        if (message->reply_serial == 0)
            return fail();
        break;
    default:
        return fail();
    }
    return true;
}

void message_release(Message* message) noexcept {
    for (u32 i = 0; i < message->fd_count; ++i)
        if (message->fds[i] >= 0)
            sys_close(message->fds[i]);
    if (message->buffer) {
        secure_clear_bytes(message->buffer, message->buffer_size);
        heap_free(message->buffer);
    }
    clear_bytes(message, static_cast<u32>(sizeof(*message)));
}

bool message_clone(Message* clone, const Message& source) noexcept {
    u8* copy = static_cast<u8*>(heap_allocate(source.buffer_size ? source.buffer_size : 1));
    if (!copy)
        return false;
    copy_bytes(copy, source.buffer, source.buffer_size);
    const u32 fd_count = source.fd_count;
    int fds[kMaxFds];
    for (u32 i = 0; i < fd_count; ++i) {
        const long duplicate = syscall3(72, source.fds[i], 1030, 0);
        if (syscall_failed(duplicate)) {
            for (u32 j = 0; j < i; ++j)
                sys_close(fds[j]);
            heap_free(copy);
            return false;
        }
        fds[i] = static_cast<int>(duplicate);
    }
    if (!message_parse(clone, copy, source.buffer_size)) {
        for (u32 i = 0; i < fd_count; ++i)
            sys_close(fds[i]);
        return false;
    }
    for (u32 i = 0; i < fd_count; ++i)
        clone->fds[i] = fds[i];
    clone->fd_count = fd_count;
    return true;
}

void message_reader(const Message& message, Reader* reader) noexcept {
    reader_init(reader, message.buffer + message.body_offset, message.body_size, message.signature, message.fds,
                message.fd_count);
}

} // namespace maicord::dbus
