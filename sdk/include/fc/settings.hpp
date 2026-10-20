#pragma once

#include <fc/base.hpp>

#include <cmath>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <typeindex>
#include <type_traits>
#include <utility>
#include <vector>

namespace fc {

// A finite collection owns keys and values; returned views borrow from this object.
class StringMap {
  public:
    // Stores one declared map binding; key matching uses ASCII case-insensitive comparison.
    struct Entry {
        std::string key, value;
    };

    // Retains declaration order; duplicate ASCII case-insensitive keys throw invalid_argument, including unused bindings.
    StringMap(std::initializer_list<Entry> entries = {}) : entries_(entries) {
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            for (std::size_t j = 0; j < i; ++j) {
                if (detail::equal_id(entries_[i].key, entries_[j].key)) {
                    throw std::invalid_argument("StringMap keys must be unique under ASCII case-insensitive comparison");
                }
            }
        }
    }

    // Looks up an ASCII case-insensitive key; missing keys return nullopt and the returned value borrows this map's storage.
    std::optional<std::string_view> find(std::string_view key) const {
        for (const auto &entry : entries_) {
            if (detail::equal_id(entry.key, key)) {
                return entry.value;
            }
        }
        return std::nullopt;
    }

    // Returns entries in declaration order; the span borrows this map's storage.
    std::span<const Entry> entries() const noexcept {
        return entries_;
    }

    // Replaces an existing default entry; a missing key throws invalid_argument.
    void assign(std::string_view key, std::string value) {
        for (auto &entry : entries_) {
            if (detail::equal_id(entry.key, key)) {
                entry.value = std::move(value);
                return;
            }
        }
        throw std::invalid_argument("StringMap setting has no declared default entry");
    }

  private:
    std::vector<Entry> entries_;
};

namespace detail {

// Convert a matching C scalar while checking numeric representation; returned strings own their bytes.
template <class T> Result<T> convert(const FC_Value &value) {
    if constexpr (std::same_as<T, bool>) {
        if (value.kind == FC_SCALAR_BOOL && value.value.boolean <= 1) {
            return value.value.boolean != 0;
        }
    } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
        if (value.kind == FC_SCALAR_SIGNED && value.value.signed_value >= std::numeric_limits<T>::min() &&
            value.value.signed_value <= std::numeric_limits<T>::max()) {
            return static_cast<T>(value.value.signed_value);
        }
    } else if constexpr (std::is_integral_v<T>) {
        if (value.kind == FC_SCALAR_UNSIGNED && value.value.unsigned_value <= std::numeric_limits<T>::max()) {
            return static_cast<T>(value.value.unsigned_value);
        }
    } else if constexpr (std::is_floating_point_v<T>) {
        if (value.kind == FC_SCALAR_REAL && std::isfinite(value.value.real) &&
            std::abs(value.value.real) <= std::numeric_limits<T>::max()) {
            return static_cast<T>(value.value.real);
        }
    } else if constexpr (std::same_as<T, std::string>) {
        if (value.kind == FC_SCALAR_TEXT && (!value.value.text.size || value.value.text.data)) {
            return std::string{view(value.value.text)};
        }
    }
    return std::unexpected(Error{"Value cannot be converted to the setting's type"});
}

// Exposes type identity and lowered field declarations while the typed model retains defaults and callbacks.
struct SchemaBase {
    virtual ~SchemaBase() = default;
    virtual std::type_index type() const noexcept = 0;
    // Returns declarations in field order; their text and callback contexts borrow the retained model's storage.
    virtual std::span<const FC_Setting> records() const = 0;
};
} // namespace detail

// Keeps one field's author metadata and the callbacks used for declaration lowering and construction.
template <class T> struct Setting {
    // Metadata copied into C declarations and generated INI descriptions; an empty section uses the patch section.
    std::string key, description, section, environment, constraints;
    // Reads the retained default as a C scalar; any returned text borrows the default or binding storage.
    std::function<FC_Value(const T &)> get;
    // A null destination validates the scalar without constructing or copying a settings object.
    std::function<Result<>(T *, const FC_Value &)> assign;
};

// Binds one field and its scalar constraints; rejected individual values use that field's retained default.
template <class T, class M> class SettingBuilder {
    using Value = std::conditional_t<std::same_as<M, StringMap>, std::string, M>;
    using Argument = std::conditional_t<std::same_as<Value, std::string>, std::string_view, Value>;

  public:
    SettingBuilder(std::string key, M T::*member, std::string description)
        : key_(std::move(key)), description_(std::move(description)), member_(member) {}

    // Places this key under [patch-id.name]; omission uses [patch-id].
    SettingBuilder &section(std::string name) {
        section_ = std::move(name);
        return *this;
    }

    // Environment input takes precedence over INI; invalid selected input falls directly to the declared default.
    SettingBuilder &environment(std::string name) {
        environment_ = std::move(name);
        return *this;
    }

    // Accepts inclusive bounds; reversed or non-finite bounds throw invalid_argument.
    SettingBuilder &range(Value minimum, Value maximum)
        requires std::is_arithmetic_v<Value>
    {
        if (minimum > maximum) {
            throw std::invalid_argument("Setting range minimum exceeds its maximum");
        }

        if constexpr (std::is_floating_point_v<Value>) {
            if (!std::isfinite(minimum) || !std::isfinite(maximum)) {
                throw std::invalid_argument("Setting range bounds must be finite");
            }
        }

        range_ = {minimum, maximum};
        return *this;
    }

    // Enum values use named text choices matched ASCII case-insensitively; empty or duplicate names are invalid.
    SettingBuilder &choices(std::initializer_list<std::pair<std::string, M>> values)
        requires std::is_enum_v<M>
    {
        // Replace the named enum mappings and check ASCII case-insensitive name uniqueness.
        choices_ = values;
        for (std::size_t i = 0; i < choices_.size(); ++i) {
            if (choices_[i].first.empty()) {
                throw std::invalid_argument("Setting choice name is empty");
            }

            for (std::size_t j = 0; j < i; ++j) {
                if (detail::equal_id(choices_[i].first, choices_[j].first)) {
                    throw std::invalid_argument("Setting choice names must be unique under ASCII case-insensitive comparison");
                }
            }
        }
        return *this;
    }

    // Sets a scalar validator run after conversion and range checks; return an error to reject the value.
    // Text arguments borrow the converted string for the duration of the callback.
    SettingBuilder &validate(std::function<Result<>(Argument)> callback) {
        validate_ = std::move(callback);
        return *this;
    }

    // Builds a field declaration whose callbacks own copies of this builder, retaining its choices and validator.
    operator Setting<T>() const {
        // Generate presentation text from the final constraints so repeated setters replace earlier declarations.
        std::string constraints;
        if constexpr (std::is_arithmetic_v<Value>) {
            if (range_) {
                constraints = "Range: " + std::to_string(range_->first) + " through " + std::to_string(range_->second) + ".";
            }
        }
        if constexpr (std::is_enum_v<M>) {
            for (const auto &choice : choices_) {
                if (!constraints.empty()) {
                    constraints += ", ";
                }
                constraints += choice.first;
            }
        }

        auto builder = *this;
        return {key_,
                description_,
                section_,
                environment_,
                std::move(constraints),
                [builder](const T &object) {
                    return builder.get(object);
                },
                [builder](T *object, const FC_Value &value) {
                    return builder.assign(object, value);
                }};
    }

  private:
    // Read map bindings and enums as text so declarations and INI use the author's named values.
    FC_Value get(const T &object) const {
        if constexpr (std::same_as<M, StringMap>) {
            auto value = (object.*member_).find(key_);
            if (!value) {
                throw std::invalid_argument("StringMap setting has no declared default entry");
            }
            return detail::scalar(*value);
        } else if constexpr (std::is_enum_v<M>) {
            for (const auto &choice : choices_) {
                if (choice.second == object.*member_) {
                    return detail::scalar(choice.first);
                }
            }
            throw std::invalid_argument("Enum default has no named choice");
        } else {
            return detail::scalar(object.*member_);
        }
    }

    // Convert every input; a null object checks constraints, while a destination receives an already validated value.
    Result<> assign(T *object, const FC_Value &input) const {
        Value value{};
        if constexpr (std::is_enum_v<M>) {
            if (input.kind != FC_SCALAR_TEXT) {
                return std::unexpected(Error{"Enum setting requires a named choice"});
            }

            // Enum text is matched ASCII case-insensitively against the retained named choices.
            bool found{};
            for (const auto &choice : choices_) {
                if (detail::equal_id(detail::view(input.value.text), choice.first)) {
                    value = choice.second;
                    found = true;
                    break;
                }
            }
            if (!found) {
                return std::unexpected(Error{"Unknown setting choice"});
            }
        } else {
            auto converted = detail::convert<Value>(input);
            if (!converted) {
                return std::unexpected(converted.error());
            }
            value = std::move(*converted);
        }

        // Admission and configuration validate scalars; construction only converts and assigns accepted values.
        if (!object) {
            if constexpr (std::is_arithmetic_v<Value>) {
                if (range_ && (value < range_->first || value > range_->second)) {
                    return std::unexpected(Error{"Value is outside the declared range"});
                }
            }
            if (validate_) {
                auto valid = validate_(value);
                if (!valid) {
                    return valid;
                }
            }
        }

        // Handler construction assigns captured values to a copy of defaults, replacing only declared map entries.
        if (object) {
            if constexpr (std::same_as<M, StringMap>) {
                (object->*member_).assign(key_, std::move(value));
            } else {
                object->*member_ = std::move(value);
            }
        }
        return {};
    }

    // Owned metadata survives conversion of a temporary builder into a Setting.
    std::string key_, description_, section_, environment_;
    // Selects the author's scalar member, or the StringMap containing the named binding.
    M T::*member_;
    // Inclusive scalar bounds, enum name mappings and optional author validation are retained by field callbacks.
    std::optional<std::pair<Value, Value>> range_;
    std::vector<std::pair<std::string, M>> choices_;
    std::function<Result<>(Argument)> validate_;
};

// Binds a scalar member; for StringMap, key selects one existing default entry.
template <class T, class M> SettingBuilder<T, M> setting(std::string key, M T::*member, std::string description = {}) {
    return {std::move(key), member, std::move(description)};
}

namespace detail {
// Owns one typed default object, field bindings and the C declarations that borrow their storage.
template <class T> class SchemaModel final : public SchemaBase {
  public:
    // Captures T's defaults and lowers field declarations; invalid map bindings or unnamed enum defaults throw.
    explicit SchemaModel(std::vector<Setting<T>> fields) : fields_(std::move(fields)) {
        // Lower defaults once; field addresses become callback contexts and stay stable with this model.
        records_.reserve(fields_.size());
        for (auto &field : fields_) {
            auto value = field.get(defaults_);
            records_.push_back({text(field.key), text(field.description), text(field.section), text(field.environment), value,
                                text(field.constraints), &field,
                                // Validates one scalar across the C ABI; reports errors and returns 1 only on success.
                                [](void *context, const FC_Value *input, FC_ErrorSink sink) -> std::uint32_t {
                                    // Author exceptions become reported validation failures at the ABI boundary.
                                    try {
                                        if (!input) {
                                            report(sink, Error{"Setting input is null"});
                                            return 0;
                                        }

                                        // A null destination checks just this scalar against its field's author constraints.
                                        const auto &field = *static_cast<Setting<T> *>(context);
                                        auto result = field.assign(nullptr, *input);
                                        if (result) {
                                            return 1;
                                        }

                                        report(sink, result.error());
                                    } catch (const std::exception &error) {
                                        report(sink, Error{error.what()});
                                    } catch (...) {
                                        report(sink, Error{"Setting validation failed"});
                                    }
                                    return 0;
                                }});
        }
    }

    std::type_index type() const noexcept override {
        return typeid(T);
    }

    std::span<const FC_Setting> records() const override {
        return records_;
    }

    // Values must match records() in count and order; construction copies defaults and assigns validated scalars.
    Result<T> construct(std::span<const FC_Value> values) const {
        T object = defaults_;
        for (std::size_t i = 0; i != fields_.size(); ++i) {
            auto result = fields_[i].assign(&object, values[i]);
            if (!result) {
                return std::unexpected(result.error());
            }
        }
        return object;
    }

  private:
    // Capture the ordinary defaults once; subsequent construction copies this retained object.
    T defaults_{};
    // Owns strings and callback contexts referenced by records_; field order also defines the factory input order.
    std::vector<Setting<T>> fields_;
    // Returned views remain valid while this model and its field storage remain alive and unchanged.
    std::vector<FC_Setting> records_;
};
} // namespace detail

// Shares retained field defaults and optionally validates the completed typed object before handler creation.
template <class T> struct TypedSettingsSchema {
    std::shared_ptr<detail::SchemaModel<T>> model;
    // May normalize the completed settings before handler construction; rejection fails the selected patch.
    Result<> (*validate)(T &){};
};

// Value-initializes T once and retains its defaults and field bindings for immutable lowering.
template <class T> TypedSettingsSchema<T> settings(std::initializer_list<Setting<T>> fields) {
    return {std::make_shared<detail::SchemaModel<T>>(std::vector<Setting<T>>{fields})};
}

// Type erasure stays inside the plugin; the runtime receives only scalar records and thunks.
struct SettingsSchema {
    // Shared ownership keeps lowered declaration pointers and the retained default object alive with the plugin.
    std::shared_ptr<const detail::SchemaBase> model;
    // Runs after scalar assignment on the completed typed object, before its handler is constructed.
    std::function<Result<>(void *)> complete;

    SettingsSchema() = default;

    // Shares the typed scalar model and adapts its completion validator for erased handler construction.
    template <class T> SettingsSchema(const TypedSettingsSchema<T> &schema) : model(schema.model) {
        // Retain the completion callback independently of the shared scalar model so schemas may customize it.
        if (schema.validate) {
            complete = [callback = schema.validate](void *object) {
                return callback(*static_cast<T *>(object));
            };
        }
    }
};
} // namespace fc
