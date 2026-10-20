#pragma once

#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>
#include <evntrace.h>
#include <evntcons.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

namespace fc::tests::reporting {

// Events used by a fixture formatter to signal its arrival and wait for permission to continue.
inline HANDLE entered{}, resume{};

// Owns the events that pause a reporting worker; keep it alive until that worker has joined.
struct WorkerGate {
    // Creates manual-reset events so formatting stays paused until the test resumes it.
    WorkerGate() {
        entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        resume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    // Releases the event handles after the reporting session has joined its worker.
    ~WorkerGate() {
        SetEvent(resume);
        CloseHandle(entered);
        CloseHandle(resume);
    }
};

// Declare after the reporting session so assertion unwinding releases the worker before the session joins it.
struct ReleaseWorker {
    // Unblocks formatting before session destruction waits for its worker.
    ~ReleaseWorker() {
        SetEvent(resume);
    }
};

// Owns the decoded producer information and payload from one ETW data event.
struct TraceRecord {
    std::uint32_t version{}, thread{};
    // Producer QPC ticks and the channel's sequence number.
    std::uint64_t timestamp{}, sequence{};
    std::vector<std::uint8_t> payload;
};

// Collects data events while ProcessTrace reads an ETL file.
struct TraceFile {
    std::vector<TraceRecord> records;
    bool decode_failed{};
};

// Collects Fusion Cutter events in event->UserContext; malformed payloads set that TraceFile's decode_failed flag.
inline void WINAPI decode_trace(EVENT_RECORD *event) noexcept {
    constexpr GUID provider{0xd5645019, 0xaf63, 0x46b9, {0x93, 0xc8, 0x91, 0x0f, 0x22, 0xa7, 0x01, 0x51}};
    if (!IsEqualGUID(event->EventHeader.ProviderId, provider)) {
        return;
    }

    auto &result = *static_cast<TraceFile *>(event->UserContext);
    try {
        if ((event->EventHeader.Flags & EVENT_HEADER_FLAG_STRING_ONLY) || event->EventHeader.EventDescriptor.Id != 2) {
            return;
        }

        // Match the event envelope written by the runtime before reading its opaque payload.
        struct Producer {
            std::uint64_t timestamp, sequence;
            std::uint32_t thread, size;
        };

        struct Header {
            std::uintptr_t channel;
            std::uint32_t version;
            Producer producer;
        };

        if (event->UserDataLength < sizeof(Header)) {
            result.decode_failed = true;
            return;
        }

        Header header{};
        std::memcpy(&header, event->UserData, sizeof(header));
        if (header.producer.size != event->UserDataLength - sizeof(Header)) {
            result.decode_failed = true;
            return;
        }

        const auto *bytes = static_cast<const std::uint8_t *>(event->UserData) + sizeof(Header);
        result.records.push_back({header.version,
                                  header.producer.thread,
                                  header.producer.timestamp,
                                  header.producer.sequence,
                                  {bytes, bytes + header.producer.size}});
    } catch (...) {
        result.decode_failed = true;
    }
}

// Reads a closed ETL file and returns its decoded events; asserts if reading or decoding fails.
inline TraceFile read_trace(const std::filesystem::path &path) {
    TraceFile result;
    auto filename = path.native();
    EVENT_TRACE_LOGFILEW logfile{};
    logfile.LogFileName = filename.data();
    logfile.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD;
    logfile.EventRecordCallback = &decode_trace;
    logfile.Context = &result;

    auto reader = OpenTraceW(&logfile);
    REQUIRE(reader != INVALID_PROCESSTRACE_HANDLE);

    // ProcessTrace calls the decoder synchronously, while its filename and result storage are still alive.
    const auto code = ProcessTrace(&reader, 1, nullptr, nullptr);
    CloseTrace(reader);

    REQUIRE(code == ERROR_SUCCESS);
    REQUIRE_FALSE(result.decode_failed);
    return result;
}

} // namespace fc::tests::reporting
