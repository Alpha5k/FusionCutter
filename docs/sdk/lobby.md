# Lobby publishing

A lobby is the multiplayer backend's record for a hosted game that players can join. In the current
integration, Galaxy owns that record and gives it a lobby ID. It is a backend object, not a
particular menu screen, and its identity is different from FC's process-local session and peer IDs.

FC's lobby service attaches named binary values to this record and reads values back. It is useful for
metadata such as a custom mode identifier or information another extension needs about the hosted game.
The publisher supplies the latest value; readers obtain a snapshot. This differs from sending an action
to a connected peer, where [multiplayer messages](multiplayer.md) are the appropriate interface.

Include `<fc/lobby.hpp>`. See the Lobby publishing row in the
[build support tables](../installation.md#feature-support). The service provides the current hosted or joined
lobby ID and accepts known lobby IDs for reads. It does not enumerate servers or join a game. A feature that
obtains other lobby IDs must do so through its own applicable integration.

Get a `Lobby` handle from `PrepareContext::lobby()`. Call every lobby operation, including destruction or
replacement of a `Request`, on the control thread.

## Publish a value

An item has a name, a payload version and bytes. The name identifies the metadata, while the version tells
readers how to interpret the bytes. For example, `Example.Mode` could store one mode number in format 1.
The version refers to that payload format, independently of the plugin's release number.

Register the name and payload version during Prepare. The returned `LobbyItem` can retain the latest value
even before a lobby is active:

```cpp
fc::Result<> prepare(fc::PrepareContext &context) {
    lobby_ = context.lobby();
    auto item = lobby_.item("Example.Mode", 1);
    if (!item) {
        return std::unexpected(item.error());
    }
    mode_ = *item;
    const auto value = fc::byte_array<2>();
    return mode_.publish(value);
}
```

The handler retains `fc::Lobby lobby_` and `fc::LobbyItem mode_`. Use a name unique to your feature and a
version describing the payload format. `publish(Bytes)` copies the whole value and returns `Result<>`.
Success means the runtime accepted the desired value, not that the multiplayer backend has already stored it.
Publication applies to a lobby owned by this process. Empty bytes are a valid value.

`clear()` withdraws the item and prevents republication until another publish. Reading a name does not
reserve it; registering an item reserves publication ownership.

Names match exactly, including case. They must be nonempty, contain no NUL and fit the backend's key limit
with FC's prefix. Each name can be registered once across all patches, regardless of payload version.
Duplicate registration returns an error. `item` can also fail outside Prepare, without backend support,
or when registration storage is unavailable. The version is a `uint32_t` chosen by the publisher and must
match the reader's requested version.

`publish` can refuse a value that exceeds storage or backend size limits, including the encoded record's
overhead. Refusal preserves the previous accepted value. A default LobbyItem cannot publish; its `clear`
does nothing. Copies of a valid LobbyItem refer to the same publication.

In the example, `item` claims publication of `Example.Mode`, and `publish` supplies one byte with value 2.
FC retains that desired value for publication in a lobby this process owns. To change the mode later, publish
the complete new value through the retained item. Calling `clear` means the metadata should no longer be
published; publishing an empty payload instead means that an item with an empty value should exist.

## Read or refresh

Reading lobby metadata can require work by the network backend. A cached read uses what FC already knows
and returns immediately. A refresh asks for a new observation and delivers its result through a callback.
Choose a cached read for a display that can show the last known value, and refresh when the feature needs
to ask again because that value may have changed.

`Lobby::current()` returns `LobbyId{value}`, with zero meaning absent or unavailable.
`cached(lobby, name, version)` returns `optional<LobbyRecord>` immediately. No cached record is different
from a cached `Missing` result.

To request a fresh read, retain the move-only Request returned by `refresh`:

```cpp
// Run on the control thread after activation, for example in response to an author action.
fc::Result<> refresh_mode() {
    auto pending = lobby_.refresh(lobby_.current(), "Example.Mode", 1,
        [this](fc::LobbyRecord record) noexcept {
            record_ = std::move(record);
        });
    if (!pending) {
        return std::unexpected(pending.error());
    }
    request_ = std::move(*pending);
    return {};
}
```

Here `request_` is `fc::Request` and `record_` is `optional<fc::LobbyRecord>`. The callback runs once on the
control thread after `refresh` returns; it is never called inline. It receives an owning copy of the record
and payload. Replacing or destroying Request cancels callback delivery and releases its runtime record,
including after completion. The backend request may continue after cancellation.

Keep Request for as long as the callback is needed. If `pending` were left as a local variable and destroyed
at the end of `refresh_mode`, its request would be canceled. Moving it into `request_` retains
it. Replacing that member later cancels the old request, which is useful when the user changes which lobby
they are inspecting before the earlier read completes.

After the example receives a record, check that its result is Value and its payload has the expected one-byte
format before interpreting the mode. Missing means a successful read found no published item; no cached
record means FC has no observation to report. Those cases can lead to different choices about refreshing or
displaying unavailable metadata.

There are two error paths for `refresh`: an unsuccessful `Result<Request>` means no request was accepted
and no callback will follow. An accepted request later supplies a LobbyRecord, whose `result` describes the
read outcome. Submission requires a nonzero LobbyId, a valid name, service support and storage for the
request. A backend that becomes unavailable can instead be reported through the completion record.

To cancel explicitly, assign `request_ = fc::Request{};` on the control thread. Move assignment also cancels
the previous request. The completion callback may replace or destroy its own Request.

## Records and results

| `LobbyRecord` field | Meaning |
| --- | --- |
| `lobby` | LobbyId assigned by the multiplayer backend |
| `result` | LobbyResult |
| `version` | Observed payload format when readable, including a payload version mismatch; no valid version on failed requests |
| `value` | Owned vector of bytes |
| `observed` | Steady-clock time of a successful read from the backend; default time on failure |

| `LobbyResult` | Meaning |
| --- | --- |
| `Value` | A valid item in the requested format |
| `Missing` | A successful read found no item |
| `Malformed` | The stored FC record could not be decoded |
| `Unsupported` | Envelope or payload format unsupported |
| `Unavailable` | Required access to the multiplayer backend is unavailable |
| `Failed` | The backend request or read failed |

When a readable record's version differs from the requested version, the result is Unsupported and the
payload is empty. `cached` may still return a previous observation after a failed refresh; use `observed`
to judge its age. A `nullopt` cached read means no record is available through this handle, including when
the service itself is unavailable.

Check `result` before interpreting bytes or timestamps. A successful read is a snapshot, not a permanent
guarantee about the lobby. `LobbyId` supports equality and identifies a lobby in the multiplayer backend.
It is distinct from Game SessionId and Multiplayer PeerId.

The complete surface is `Lobby::item`, `current`, `cached`, `refresh`; `LobbyItem::publish`, `clear`; and
move construction, move assignment and destruction of `Request`. There is no continuous subscription in
this interface; decide when your feature needs another refresh.
