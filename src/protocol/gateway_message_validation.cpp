#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/protocol/base64.hpp"

// Message validators shared by the client and the account module. The JSON
// message decoders live only in the account module (account_module_decoders.cpp);
// these are in both images because the client's mailbox readers and stores call
// them on every record.
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

bool leap_year(u32 year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

i64 civil_days(u32 year, u32 month, u32 day) noexcept {
    i64 adjusted_year = static_cast<i64>(year) - (month <= 2 ? 1 : 0);
    const i64 era = adjusted_year / 400;
    const u32 year_of_era = static_cast<u32>(adjusted_year - era * 400);
    const u32 adjusted_month = month > 2 ? month - 3 : month + 9;
    const u32 day_of_year = (153 * adjusted_month + 2) / 5 + day - 1;
    const u32 day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146'097 + static_cast<i64>(day_of_era) - 719'468;
}

} // namespace

bool gateway_message_waveform_valid(Utf8View waveform) noexcept {
    if (waveform.bytes > kGatewayMessageWaveformMaximumBytes || (!waveform.data && waveform.bytes))
        return false;
    const auto result = base64_decode(waveform, {});
    return (result.ok() || result.error == Base64Error::output_too_small) &&
           result.bytes <= kGatewayMessageWaveformMaximumSamples;
}

bool gateway_message_poll_valid(const GatewayMessagePoll& poll) noexcept {
    constexpr u32 allowed = gateway_message_poll_field_question | gateway_message_poll_field_answers |
                            gateway_message_poll_field_expiry | gateway_message_poll_field_multiselect |
                            gateway_message_poll_field_layout | gateway_message_poll_field_results;
    const auto bounded = [](Utf8View value, u32 byte_limit, u32 scalar_limit) noexcept {
        if ((!value.data && value.bytes) || value.bytes > byte_limit)
            return false;
        const auto valid = utf8_validate(value);
        return valid.valid && valid.codepoints <= scalar_limit;
    };
    if (!poll.present)
        return !poll.fields && !poll.question.bytes && !poll.answers.count && !poll.answer_counts.count &&
               !poll.expiry_ms && !poll.layout_type && !poll.allow_multiselect && !poll.expiry_null &&
               !poll.results_known && !poll.is_finalized;
    if (!poll.fields || (poll.fields & ~allowed) ||
        poll.answers.count > kGatewayMessagePollMaximumAnswers ||
        poll.answer_counts.count > kGatewayMessagePollMaximumAnswers ||
        (!poll.answers.data && poll.answers.count) ||
        (!poll.answer_counts.data && poll.answer_counts.count) ||
        !bounded(poll.question, kGatewayMessagePollQuestionMaximumBytes,
                 kGatewayMessagePollQuestionMaximumScalars) ||
        (!(poll.fields & gateway_message_poll_field_question) && poll.question.bytes) ||
        (!(poll.fields & gateway_message_poll_field_answers) && poll.answers.count) ||
        (!(poll.fields & gateway_message_poll_field_expiry) && (poll.expiry_ms || poll.expiry_null)) ||
        (poll.expiry_null && poll.expiry_ms) ||
        (!(poll.fields & gateway_message_poll_field_multiselect) && poll.allow_multiselect) ||
        (!(poll.fields & gateway_message_poll_field_layout) && poll.layout_type != 0) ||
        (((poll.fields & gateway_message_poll_field_results) != 0) != poll.results_known) ||
        (!poll.results_known && (poll.answer_counts.count || poll.is_finalized)))
        return false;
    for (u32 i = 0; i < poll.answers.count; ++i) {
        const auto& answer = poll.answers[i];
        if (!answer.answer_id ||
            !bounded(answer.text, kGatewayMessagePollAnswerMaximumBytes,
                     kGatewayMessagePollAnswerMaximumScalars) ||
            !bounded(answer.emoji_name, kGatewayMessagePollEmojiMaximumBytes,
                     kGatewayMessagePollEmojiMaximumBytes))
            return false;
        for (u32 j = 0; j < i; ++j)
            if (poll.answers[j].answer_id == answer.answer_id)
                return false;
    }
    for (u32 i = 0; i < poll.answer_counts.count; ++i) {
        const auto& count = poll.answer_counts[i];
        if (!count.answer_id)
            return false;
        for (u32 j = 0; j < i; ++j)
            if (poll.answer_counts[j].answer_id == count.answer_id)
                return false;
        if (poll.fields & gateway_message_poll_field_answers) {
            bool found = false;
            for (u32 j = 0; j < poll.answers.count; ++j)
                found |= count.answer_id == poll.answers[j].answer_id;
            if (!found)
                return false;
        }
    }
    return true;
}

bool parse_iso_timestamp(JsonString value, u64* timestamp_ms) noexcept {
    const JsonStringDecodeResult size = json_string_decode_utf8(value, Slice<char>{nullptr, 0});
    if (size.error != JsonStringDecodeError::output_too_small || size.bytes < 20 || size.bytes > 40) {
        return false;
    }
    char data[40]{};
    const JsonStringDecodeResult decoded = json_string_decode_utf8(value, Slice<char>{data, 40});
    if (!decoded.ok()) {
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
    constexpr u8 month_days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    u32 maximum_day = month_days[month - 1];
    if (month == 2 && leap_year(year)) {
        ++maximum_day;
    }
    if (day == 0 || day > maximum_day) {
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

    const i64 days = civil_days(year, month, day);
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
