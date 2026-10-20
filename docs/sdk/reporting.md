# Logging, status and tracing

Include `<fc/reporting.hpp>` or obtain reporting through the stage contexts. Use logs for events and reasons,
status for the current state, and tracing for high-volume diagnostic records.

These facilities answer different questions during a problem report. Logs explain what happened. Status
shows what the plugin is doing now. Traces preserve detailed measurements for later analysis. FC supplies
the shared files and associates output with its patch, so each author does not need a separate file writer.

For a network feature, choose the output according to the question:

| Question | Useful output |
| --- | --- |
| Why did the connection stop? | Log the failed operation and its reason |
| Is it connected now, and how much has it received? | Current status fields |
| Where are packet delays occurring? | Trace records containing sequence numbers, sizes and timing |

Writing every packet as an Info message would make ordinary events hard to find. Showing only a current
counter would lose the history needed to investigate timing. A feature can use all three without repeating
the same information in every output.

## Logger

Every stage, including Plan, provides `log()`. Retain the returned `Logger` when callbacks need it; messages
remain associated with the patch. Default-constructed loggers have no runtime destination.

A Logger is access to FC's shared log, with the originating patch already attached. Use it to report the
operation and relevant facts; FC supplies the surrounding timestamp, level and attribution. A retained logger
can be captured by a callback, so diagnostic output need not depend on keeping its stage context alive.

```cpp
auto log = context.log();
log.info("Connected to host");
log.warning("Requested rate {} is unavailable; using {}", requested, selected);
log.error("Could not open device: {}", reason);
if (log.enabled(fc::LogLevel::Debug)) {
    log.debug("Peer {} queued {} bytes", peer.value, byte_count);
}
```

| Method | Contract |
| --- | --- |
| `enabled(LogLevel)` | Whether the runtime's configured filter admits this severity |
| `write(level, text)` | Copy already formatted text; true means queued |
| `debug`, `info`, `warning`, `error` | Checked format string with scalar/text arguments; true means queued |

`LogLevel` contains Off, Error, Warning, Info and Debug. The default runtime threshold is Warning.
Formatting helpers accept integers, floating-point values and strings/string views/C strings. Convert enums
to meaningful text yourself. Use `write` with preformatted text for other types. Arguments are evaluated
before the call, so use `enabled` before expensive diagnostic work.

The helpers copy arguments for formatting on the reporting worker. They return false for filtering or
refusal. A record, including runtime framing, must fit within 4 KiB. A true return confirms queue acceptance,
not a completed disk write. Keep large or frequent payloads in trace channels.

Use Debug for selection details and state useful during diagnosis, Info for meaningful normal events,
Warning for a problem with a usable fallback, and Error for failure of an intended operation. The runtime
already reports patch lifecycle failures; avoid logging the same error immediately before returning it.
Include the affected operation and useful facts. Follow the repository's [prose rules](../../STYLE.md#prose).

## Status

Implement `void write_status(fc::StatusContext &) const noexcept`. `line(text)` and
`line(format, arguments...)` append complete lines and return false if formatting or submission fails.
The runtime places nonempty output under that patch's **Patch Status** entry.

FC collects Status again as the process runs. Each callback should describe the current state afresh. A
player looking at `FusionCutter.txt` can then answer whether the feature is active or waiting for something
without reconstructing its entire log history. Keep ongoing measurements in the handler and present them
here; the callback itself should not start new network work or repeat installation.

```cpp
void write_status(fc::StatusContext &status) const noexcept {
    status.line("Connected peers: {}", connected_peers_);
    status.line("Messages received: {}", messages_received_);
}
```

Show current information that helps a player or diagnosis. The runtime already reports installation state,
disabled patches and failures. Avoid repeating those facts or adding a timeline to every status refresh.
The writer expires on return; compute expensive observations elsewhere and report their current values.

## Trace channels

Create channels during Prepare with `context.trace(TraceDefinition)`. The runtime writes all channels into
one Event Tracing for Windows (ETW) file for the session.

A trace channel identifies a series of binary records with an author-defined payload format. For example,
a `Packets` channel can contain a sequence number and byte count per packet. Several channels can describe
different measurements, while their events remain in the same session file for comparison.

ETW is Windows' event tracing facility. Its `.etl` file holds events and metadata rather than a text log.
FC handles recording; the author chooses the payload and the tools that decode it. The channel format version
allows a decoder to distinguish changes in that payload layout.

```cpp
#include <fc/plugin.hpp>

struct Sample {
    std::uint32_t sequence;
    std::uint32_t byte_count;
};

class TraceExample {
  public:
    // Creates the channel before callbacks begin recording packets.
    fc::Result<> prepare(fc::PrepareContext &context) {
        auto channel = context.trace({"Packets", 1, 64 * 1024});
        if (!channel) {
            return std::unexpected(channel.error());
        }
        trace_ = *channel;
        return {};
    }

    // Queues the packet's sequence number and size for diagnostic output.
    bool record(std::uint32_t sequence, std::uint32_t bytes) const noexcept {
        return trace_.write(Sample{sequence, bytes});
    }

  private:
    fc::TraceChannel trace_;
};
```

`TraceDefinition` has `name` (unique within the patch), `format_version` (your payload format), and
`buffer_bytes` (requested queue bytes, including framing). Choose the buffer for the collection rate and
acceptable loss. The runtime's `MaxTraceSizeMB` separately limits the session file.

Creation returns `Result<TraceChannel>`. Names must be nonempty UTF-8 without ASCII control characters;
duplicates within the same patch are rejected. `buffer_bytes` must be nonzero.
Creation can also fail while allocating the queue or starting the reporting worker.
With tracing disabled by configuration, a valid declaration still succeeds and returns a disabled channel.

The example creates a channel during Prepare and retains it for later calls to `record`. An accepted call
copies that sample into FC's queue; it does not capture packets automatically. The patch calls it at the
measurement point it wants to observe. Changing the record's fields requires keeping the decoder and format
version consistent with the new representation.

| `TraceChannel` method | Meaning |
| --- | --- |
| `enabled()` | The channel accepts recording and output remains available |
| `write(Bytes)` | Copy one payload; true means accepted into the queue |
| `write(record)` | Copy a standard-layout, trivially copyable object's raw bytes |
| `health()` | Copy TraceHealth counters and output flags |

The object overload includes padding and pointer values. Use self-contained records with defined fields
and initialized bytes; pointers are not useful retained data. Payloads above 60 KiB are refused. Channel
copies share storage and counters. A retained handle remains valid even if failed preparation disables it.

`enabled`, both `write` overloads and `health` can be used from producer threads such as hook callbacks.
An enabled check does not reserve queue space: `write` may still return false. A default channel is disabled,
refuses writes and reports zero health values.

`TraceHealth` reports `accepted` (copied to queue), `written` (accepted by ETW), `dropped`, `output_failed`
and `file_limit_reached`. Written does not independently confirm disk persistence. Disabled, malformed or
oversized submissions are refused without counting as valid lost records. Queue or output losses are counted.

The runtime supplies session/channel metadata and record timing. You define the payload format and the
tools that interpret it. See [troubleshooting](../troubleshooting.md) for output filenames and tracing states.

## Fatal errors

`fc::fatal(Error)` reports a fatal condition and terminates the process. It never returns; if reporting is
unavailable, it aborts directly. Use normal Results for expected startup failures. Fatal is for conditions
where continuing installed code is invalid, not a substitute for a recoverable return value.
