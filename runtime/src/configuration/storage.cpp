#include "storage.hpp"

#include <fc/base.hpp>

#include <Windows.h>

#include <algorithm>
#include <climits>
#include <iterator>
#include <optional>
#include <unordered_set>

namespace fc::runtime::configuration_detail {

namespace {

// Appends each description line with an INI comment prefix, trimming surrounding spaces, tabs and carriage returns.
void comment_lines(std::string &output, std::string_view value) {
    while (!value.empty()) {
        const auto end = value.find('\n');
        output += "; " + std::string{trim(value.substr(0, end))} + "\n";
        if (end == std::string_view::npos) {
            break;
        }
        value.remove_prefix(end + 1);
    }
}

// Inserts grouped missing keys into matching sections, retaining every byte of the existing input.
std::optional<std::string> updated_contents(const IniDocument &document, std::string_view contents,
                                            std::span<const GeneratedEntry> entries) {
    struct SectionAdditions {
        std::string section, text;
        bool inserted{};
    };

    std::vector<SectionAdditions> sections;
    for (const auto &entry : entries) {
        if (find(document, entry.section, entry.key) ||
            std::ranges::contains(document.commented_keys, identity(entry.section, entry.key))) {
            continue;
        }

        auto section = std::ranges::find_if(sections, [&](const auto &value) {
            return fc::detail::equal_id(value.section, entry.section);
        });
        if (section == sections.end()) {
            sections.push_back({entry.section});
            section = std::prev(sections.end());
        }
        auto &text = section->text;
        comment_lines(text, entry.description);
        // An unset key lets differing variant defaults survive until build selection; shared defaults become assignments.
        if (entry.variant_default) {
            text += "; Leave unset to use the default for the selected game build.\n; " + entry.key + " =\n";
        } else {
            comment_lines(text, entry.constraints);
            text += entry.key + " = " + format_scalar(entry.value) + "\n";
        }
    }
    if (sections.empty()) {
        return {};
    }

    std::string result;
    // Finish the first matching section before its next header, including files with repeated sections.
    auto insert = [&](std::string_view name) {
        for (auto &section : sections) {
            if (!section.inserted && fc::detail::equal_id(section.section, name)) {
                if (!result.empty() && result.back() != '\n') {
                    result += '\n';
                }
                result += section.text;
                section.inserted = true;
            }
        }
    };
    std::string_view current_section;
    while (!contents.empty()) {
        const auto end = contents.find('\n');
        const auto line = contents.substr(0, end == std::string_view::npos ? contents.size() : end + 1);
        const auto value = trim(contents.substr(0, end));
        if (value.starts_with('[')) {
            insert(current_section);
            current_section = trim(value.substr(1, value.size() - 2));
        }
        result += line;
        contents.remove_prefix(line.size());
    }
    insert(current_section);

    // Sections absent from the input follow the schema's existing section and key order.
    for (const auto &section : sections) {
        if (!section.inserted) {
            if (!result.empty() && result.back() != '\n') {
                result += '\n';
            }
            if (!result.empty()) {
                result += '\n';
            }
            result += "[" + section.section + "]\n" + section.text;
        }
    }
    return result;
}

} // namespace

std::expected<IniDocument, Diagnostic> load_file(const std::filesystem::path &path, std::span<const GeneratedEntry> entries, bool write,
                                                 std::vector<Diagnostic> &warnings) {
    const auto owner = path_text(path);

    // OPEN_EXISTING distinguishes a missing file from permission and device failures.
    auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const auto code = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    const bool missing = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
    if (handle == INVALID_HANDLE_VALUE && !missing) {
        return std::unexpected(Diagnostic{owner, "Could not open the configuration file for reading", code});
    }

    IniDocument document;
    std::string contents;
    // Reject the parser's Windows text limit before allocating, then read the accepted file in one request.
    if (!missing) {
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(handle, &length) || length.QuadPart < 0 || length.QuadPart > INT_MAX ||
            static_cast<std::uint64_t>(length.QuadPart) > contents.max_size()) {
            CloseHandle(handle);
            return std::unexpected(Diagnostic{owner, "Configuration file size could not be read or exceeds the supported text size"});
        }

        contents.resize(static_cast<std::size_t>(length.QuadPart), '\0');
        if (!contents.empty()) {
            DWORD received{};
            const auto size = static_cast<DWORD>(contents.size());
            const auto success = ReadFile(handle, contents.data(), size, &received, nullptr);
            if (!success || received != size) {
                const auto error = success ? ERROR_HANDLE_EOF : GetLastError();
                CloseHandle(handle);
                return std::unexpected(Diagnostic{owner, "Could not read the complete configuration file", error});
            }
        }
        CloseHandle(handle);

        // Structure failures stop loading before generated additions can modify the existing file.
        auto parsed = parse_ini(contents, owner);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        document = std::move(*parsed);
    }

    // Skip files with no missing entries; exclusive creation protects existing paths while updates are staged beside them.
    const auto updated = write ? updated_contents(document, contents, entries) : std::nullopt;
    if (updated) {
        auto output_path = path;
        if (!missing) {
            output_path += L".tmp";
        }
        handle = CreateFileW(output_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        bool success = handle != INVALID_HANDLE_VALUE;
        std::size_t offset{};
        // Complete the staged output before replacing any existing user input.
        while (success && offset < updated->size()) {
            DWORD written{};
            success = WriteFile(handle, updated->data() + offset,
                                static_cast<DWORD>(std::min<std::size_t>(updated->size() - offset, MAXDWORD)), &written, nullptr) &&
                      written != 0;
            offset += written;
        }

        if (success) {
            success = FlushFileBuffers(handle) != 0;
        }
        auto error = success ? 0 : GetLastError();
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
        if (success && !missing) {
            success = ReplaceFileW(path.c_str(), output_path.c_str(), nullptr, 0, nullptr, nullptr) != 0;
            error = success ? 0 : GetLastError();
        }
        // A failed new file rejects loading and attempts to remove partial output.
        // Update failures preserve usable existing input and remove only output created by this load.
        if (!success) {
            if (handle != INVALID_HANDLE_VALUE) {
                DeleteFileW(output_path.c_str());
            }
            if (missing) {
                return std::unexpected(Diagnostic{owner, "Could not create the missing configuration file", error});
            }
            warnings.push_back({owner, "Could not add new entries to the configuration file; using existing values and defaults", error});
        }
    }

    // Return only the input parsed before writing so this load still uses each variant's own declaration defaults.
    return document;
}

void warn_unknown(const IniDocument &document, std::span<const GeneratedEntry> entries, std::vector<Diagnostic> &warnings,
                  std::string_view owner) {
    // Repeated spellings of the same unknown section produce one warning.
    std::unordered_set<std::string> warned;
    for (const auto &section : document.sections) {
        if (std::ranges::none_of(entries,
                                 [&](const auto &e) {
                                     return fc::detail::equal_id(e.section, section);
                                 }) &&
            warned.insert("section:" + fc::detail::folded_id(section)).second) {
            warnings.push_back({std::string{owner}, "Unknown configuration section [" + section + "] was preserved"});
        }
    }

    // Known sections can still contain unknown keys; compare their full section/key identity against the schema.
    for (const auto &entry : document.entries) {
        if (std::ranges::none_of(entries, [&](const auto &e) {
                return fc::detail::equal_id(e.section, entry.section) && fc::detail::equal_id(e.key, entry.key);
            })) {
            warnings.push_back({std::string{owner}, "Unknown configuration key [" + entry.section + "]." + entry.key + " was preserved"});
        }
    }
}

} // namespace fc::runtime::configuration_detail
