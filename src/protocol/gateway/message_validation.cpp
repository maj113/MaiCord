#include "maicord/protocol/gateway/message_events.hpp"
#include "maicord/protocol/encoding/base64.hpp"
#include "maicord/core/time.hpp"

// Message validators: the message decoders, the rich message store and the attachment upload call them.
namespace maicord {
namespace {

bool parse_two_digits(const char* data, u32 offset, u32* value) noexcept {
    if (data[offset] < '0' || data[offset] > '9' || data[offset + 1] < '0' || data[offset + 1] > '9') {
        return false;
    }
    *value = static_cast<u32>(data[offset] - '0') * 10u + static_cast<u32>(data[offset + 1] - '0');
    return true;
}

bool parse_four_digits(const char* data, u32* value) noexcept {
    u32 high = 0;
    u32 low = 0;
    if (!parse_two_digits(data, 0, &high) || !parse_two_digits(data, 2, &low)) {
        return false;
    }
    *value = high * 100u + low;
    return true;
}

} // namespace

bool gateway_message_waveform_valid(Utf8View waveform) noexcept {
    if (waveform.bytes > kGatewayMessageWaveformMaximumBytes || (!waveform.data && waveform.bytes))
        return false;
    const auto result = base64_decode(waveform, {});
    return (result.ok() || result.error == Base64Error::output_too_small) &&
           result.bytes <= kGatewayMessageWaveformMaximumSamples;
}

bool parse_iso_timestamp(const JsonString& value, u64* timestamp_ms) noexcept {
    char data[40];
    const JsonStringDecodeResult decoded = json_string_decode_utf8(value, Slice<char>{data, 40});
    if (!decoded.ok() || decoded.bytes < 20) {
        return false;
    }
    const u32 count = decoded.bytes;
    if (data[4] != '-' || data[7] != '-' || data[10] != 'T' || data[13] != ':' || data[16] != ':') {
        return false;
    }
    u32 year = 0;
    u32 month = 0;
    u32 day = 0;
    u32 hour = 0;
    u32 minute = 0;
    u32 second = 0;
    if (!parse_four_digits(data, &year) || !parse_two_digits(data, 5, &month) ||
        !parse_two_digits(data, 8, &day) || !parse_two_digits(data, 11, &hour) ||
        !parse_two_digits(data, 14, &minute) || !parse_two_digits(data, 17, &second) || year < 1970 ||
        month == 0 || month > 12 || hour > 23 || minute > 59 || second > 59) {
        return false;
    }
    if (day == 0 || day > civil_month_days(year, month)) {
        return false;
    }

    u32 offset = 19;
    u32 fraction_ms = 0;
    if (offset < count && data[offset] == '.') {
        ++offset;
        const u32 fraction_begin = offset;
        u32 digits = 0;
        while (offset < count && data[offset] >= '0' && data[offset] <= '9') {
            if (digits < 3) {
                fraction_ms = fraction_ms * 10u + static_cast<u32>(data[offset] - '0');
            }
            ++digits;
            ++offset;
        }
        if (offset == fraction_begin || digits > 9) {
            return false;
        }
        while (digits < 3) {
            fraction_ms *= 10u;
            ++digits;
        }
    }

    i64 timezone_minutes = 0;
    if (offset < count && data[offset] == 'Z') {
        ++offset;
    } else if (offset + 6 == count && (data[offset] == '+' || data[offset] == '-') &&
               data[offset + 3] == ':') {
        u32 timezone_hour = 0;
        u32 timezone_minute = 0;
        if (!parse_two_digits(data, offset + 1, &timezone_hour) ||
            !parse_two_digits(data, offset + 4, &timezone_minute) || timezone_hour > 23 ||
            timezone_minute > 59) {
            return false;
        }
        timezone_minutes = static_cast<i64>(timezone_hour * 60u + timezone_minute);
        if (data[offset] == '-') {
            timezone_minutes = -timezone_minutes;
        }
        offset += 6;
    } else {
        return false;
    }
    if (offset != count) {
        return false;
    }

    const i64 days = civil_days_from_date(year, month, day);
    i64 milliseconds = days * 86'400'000ll + static_cast<i64>(hour) * 3'600'000ll +
                       static_cast<i64>(minute) * 60'000ll + static_cast<i64>(second) * 1'000ll +
                       fraction_ms;
    milliseconds -= timezone_minutes * 60'000ll;
    if (milliseconds < 0) {
        return false;
    }
    *timestamp_ms = static_cast<u64>(milliseconds);
    return true;
}

} // namespace maicord
