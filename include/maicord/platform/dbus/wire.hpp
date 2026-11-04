#pragma once

#include "maicord/core/base.hpp"

// The D-Bus wire format (Linux): marshalling of the full type system (y b n q i u x t d s o g a v h ( ) { }, little
// endian, the only byte order this client sends or accepts), message headers, and the Builder and Reader that write and
// walk a message body. No allocation except the Builder's growing buffer and a received Message's one block.
//
// Alignment is relative to the start of the message; a body starts on an 8-byte boundary, so the Builder and Reader count
// offsets from the start of the body. Every Reader function that fails leaves the reader failed (sticky) and returns
// false or a zero value; a Builder that cannot grow or is misused is failed the same way and the message is not sent.

namespace maicord::dbus {

inline constexpr u32 kMaxMessageBytes = 16u << 20;  // the specification allows 128 MiB; nothing here sends or wants more
inline constexpr u32 kMaxFds = 16;
inline constexpr u32 kMaxDepth = 16;
inline constexpr u32 kMaxSignature = 255;

enum class MessageType : u8 { invalid = 0, method_call = 1, method_return = 2, error = 3, signal = 4 };

enum MessageFlag : u8 {
    flag_no_reply_expected = 1,
    flag_no_auto_start = 2,
    flag_allow_interactive_authorization = 4,
};

// Builder ------------------------------------------------------------------------------------------------------------

struct Builder {
    u8* data;
    u32 size;
    u32 capacity;
    bool failed;
    char signature[kMaxSignature + 1];  // of the top-level values appended so far
    u32 signature_length;
    int fds[kMaxFds];  // duplicates, owned until the message is sent or the builder released
    u32 fd_count;
    struct Frame {
        u8 kind;  // 'a' array, '(' struct, '{' dict entry, 'v' variant
        u32 length_position;  // of an array's length word
        u32 data_start;       // of an array's first element
    } frames[kMaxDepth];
    u32 depth;
};

void builder_init(Builder* builder) noexcept;
// Frees the buffer and closes any descriptors the message did not take.
void builder_release(Builder* builder) noexcept;
inline bool builder_ok(const Builder& builder) noexcept {
    return !builder.failed && builder.depth == 0;
}

void builder_byte(Builder* builder, u8 value) noexcept;
void builder_bool(Builder* builder, bool value) noexcept;
void builder_i16(Builder* builder, i16 value) noexcept;
void builder_u16(Builder* builder, u16 value) noexcept;
void builder_i32(Builder* builder, i32 value) noexcept;
void builder_u32(Builder* builder, u32 value) noexcept;
void builder_i64(Builder* builder, i64 value) noexcept;
void builder_u64(Builder* builder, u64 value) noexcept;
void builder_double(Builder* builder, double value) noexcept;
void builder_string(Builder* builder, const char* text, u32 length) noexcept;
void builder_string(Builder* builder, const char* text) noexcept;  // NUL terminated
void builder_object_path(Builder* builder, const char* path) noexcept;
void builder_signature(Builder* builder, const char* signature) noexcept;
// Duplicates `fd`; the message carries the duplicate (SCM_RIGHTS) and the caller keeps its own.
void builder_unix_fd(Builder* builder, int fd) noexcept;

// An array of `element_signature` ("s", "{sv}", "(ii)", "ay", ...); append the elements, then close.
void builder_open_array(Builder* builder, const char* element_signature) noexcept;
void builder_close_array(Builder* builder) noexcept;
// A struct whose complete signature is `full_signature`, "(oayays)" for instance; append its members, then close.
void builder_open_struct(Builder* builder, const char* full_signature) noexcept;
void builder_close_struct(Builder* builder) noexcept;
// One entry of a dictionary array: append the key, then the value.
void builder_open_entry(Builder* builder) noexcept;
void builder_close_entry(Builder* builder) noexcept;
// A variant holding one value of `value_signature`; append it, then close.
void builder_open_variant(Builder* builder, const char* value_signature) noexcept;
void builder_close_variant(Builder* builder) noexcept;
// Raw bytes as `ay`.
void builder_byte_array(Builder* builder, const u8* bytes, u32 count) noexcept;

// Reader -------------------------------------------------------------------------------------------------------------

struct Reader {
    const u8* data;
    u32 size;
    u32 position;
    const char* signature;  // the signature in force: the message's, or a variant's
    u32 signature_position;
    bool failed;
    const int* fds;
    u32 fd_count;
    struct Frame {
        u8 kind;                     // 'a', '(' (struct or entry) or 'v'
        u32 end;                     // of an array's elements
        const char* saved_signature;
        u32 saved_position;          // in the saved signature: just after the container's type
        u32 element_start;           // in `signature`: an array element's type
    } frames[kMaxDepth];
    u32 depth;
};

void reader_init(Reader* reader, const u8* body, u32 size, const char* signature, const int* fds, u32 fd_count) noexcept;
// The type code of the next value, or 0 at the end of the message, the struct or the array.
char reader_type(const Reader& reader) noexcept;
inline bool reader_more(const Reader& reader) noexcept {
    return !reader.failed && reader_type(reader) != 0;
}

bool reader_byte(Reader* reader, u8* value) noexcept;
bool reader_bool(Reader* reader, bool* value) noexcept;
bool reader_i16(Reader* reader, i16* value) noexcept;
bool reader_u16(Reader* reader, u16* value) noexcept;
bool reader_i32(Reader* reader, i32* value) noexcept;
bool reader_u32(Reader* reader, u32* value) noexcept;
bool reader_i64(Reader* reader, i64* value) noexcept;
bool reader_u64(Reader* reader, u64* value) noexcept;
bool reader_double(Reader* reader, double* value) noexcept;
// A string, object path or signature: points into the message, NUL terminated; *length excludes the terminator.
bool reader_string(Reader* reader, const char** text, u32* length) noexcept;
// A UNIX_FD: the descriptor the message carries at that index (owned by the message, not duplicated).
bool reader_unix_fd(Reader* reader, int* fd) noexcept;
bool reader_enter_array(Reader* reader) noexcept;
bool reader_enter_struct(Reader* reader) noexcept;  // a struct or a dict entry
bool reader_enter_variant(Reader* reader, const char** value_signature) noexcept;
// Leaves the container entered last; what was not read in it is skipped.
bool reader_leave(Reader* reader) noexcept;
// Skips one complete value, whatever its type.
bool reader_skip(Reader* reader) noexcept;
// A whole `ay` as bytes in the message.
bool reader_byte_array(Reader* reader, const u8** bytes, u32* count) noexcept;

// Messages -----------------------------------------------------------------------------------------------------------

struct Message {
    u8* buffer;  // the whole message; every string below points into it
    u32 buffer_size;
    MessageType type;
    u8 flags;
    u32 serial;
    u32 reply_serial;  // method_return and error
    const char* path;
    const char* interface;
    const char* member;
    const char* error_name;
    const char* destination;
    const char* sender;
    const char* signature;  // of the body; "" when empty
    u32 body_offset;
    u32 body_size;
    int fds[kMaxFds];  // received descriptors, owned by the message
    u32 fd_count;
};

// What a message to send says; fields a type does not use are null.
struct MessageHeader {
    MessageType type;
    u8 flags;
    u32 serial;
    u32 reply_serial;
    const char* path;
    const char* interface;
    const char* member;
    const char* error_name;
    const char* destination;
};

// Serialises header and body into one heap block (little endian). False when a required field is missing, the body
// failed, or the message would be too large. The descriptors of the body are not part of the block.
bool message_serialize(const MessageHeader& header, const Builder& body, u8** block, u32* bytes) noexcept;
// Parses a complete message; takes ownership of `buffer` (released by message_release, also on failure) and checks the
// header: byte order, version, lengths, the fields each type requires.
bool message_parse(Message* message, u8* buffer, u32 size) noexcept;
// The size of the message that starts at `bytes`, from its first 16 bytes: 0 when fewer than 16 are available (`count`),
// kMaxMessageBytes + 1 for a header that cannot be a message (not little endian, wrong version, too large). The message
// is complete when `count` reaches the size.
u32 message_size_at(const u8* bytes, u32 count) noexcept;
void message_release(Message* message) noexcept;  // frees the block and closes descriptors still held
bool message_clone(Message* clone, const Message& source) noexcept;
void message_reader(const Message& message, Reader* reader) noexcept;

// The number of bytes a signature's first complete type takes in the signature; 0 for an invalid one.
u32 signature_type_length(const char* signature) noexcept;
// Whether `signature` is a valid signature of complete types (at most kMaxSignature bytes, depth limits respected).
bool signature_valid(const char* signature) noexcept;

} // namespace maicord::dbus
