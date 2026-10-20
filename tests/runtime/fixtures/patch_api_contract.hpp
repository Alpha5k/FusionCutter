#pragma once

#include <fc/patch_api.hpp>

namespace fc::tests::api {

// Shared by separately compiled provider and consumer DLLs to test record layout compatibility.
struct Record {
    float position[3];
    std::uint32_t tag;
};

// Reuses the record ID with a different field layout to exercise connection rejection.
struct ChangedRecord {
    float position[4];
};

// Describe both records under the same ID so connection checks must compare their field layouts.
inline auto fc_description(std::type_identity<Record>) {
    return fc::describe<Record>("fixture.record.v1", &Record::position, &Record::tag);
}

inline auto fc_description(std::type_identity<ChangedRecord>) {
    return fc::describe<ChangedRecord>("fixture.record.v1", &ChangedRecord::position);
}

using Dispatch = fc::Function<void(fc::ConsumerCallback<void() noexcept>) noexcept>;

// Exposes a counter operation and a provider-thread call back into the consumer.
struct Table {
    fc::Function<std::uint32_t(Record) noexcept> count;
    Dispatch dispatch;
};

// Keeps the API's function roles but changes the counter's record argument layout.
struct ChangedLayout {
    fc::Function<std::uint32_t(ChangedRecord) noexcept> count;
    Dispatch dispatch;
};

// Keeps the record layout but changes the counter's return type.
struct ChangedSignature {
    fc::Function<std::uint64_t(Record) noexcept> count;
    Dispatch dispatch;
};

// Equal table sizes leave the serialized record layout and function signature as the distinguishing evidence.
inline const auto contract = fc::api<Table>("fixture.counter.v1", &Table::count, &Table::dispatch);
inline const auto changed_layout = fc::api<ChangedLayout>("fixture.counter.v1", &ChangedLayout::count, &ChangedLayout::dispatch);
inline const auto changed_signature =
    fc::api<ChangedSignature>("fixture.counter.v1", &ChangedSignature::count, &ChangedSignature::dispatch);
static_assert(sizeof(Table) == sizeof(ChangedLayout) && sizeof(Table) == sizeof(ChangedSignature));
} // namespace fc::tests::api
