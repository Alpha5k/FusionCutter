# Lua multiplayer messaging

Lua multiplayer lets game scripts exchange values between a host and clients. A mod could send a custom
mode request to the host and broadcast the accepted state back to its scripts. FC handles value encoding,
feature compatibility and delivery to the receiving Lua VM. The script defines the meaning of each channel
and validates requests according to the mod's rules.

The `core.lua_multiplayer` patch exposes `FusionCutter.Multiplayer` in supported Lua states. It depends on
`core.lua_bindings` and uses the runtime's `FC.LuaMessages` feature over Galaxy. See the Lua multiplayer row
in the [build support tables](../installation.md#feature-support). A C++ patch that requires this service
should declare that dependency.

Scripts exchange values on named channels. A channel is a nonempty UTF-8 string, matched exactly, including
case. Use a name owned by your mod. Channel names route script messages; they are separate from multiplayer
feature IDs and tracing channels.

## What must be ready before sending

The participating processes need the supported game build and Core's Lua bindings and multiplayer patches
enabled. The mod's scripts must run in their intended VMs and register the channels they receive. Installing
FC's runtime service does not load the mod's scripts or create their listeners automatically.

There are two kinds of readiness. Multiplayer feature agreement establishes that the remote process supports
FC's Lua messaging protocol. Listener announcements establish that a script in that process currently
receives a particular channel. `Ready(peer, channel)` checks both, so a compatible peer can still be unready
for a script that has not registered or whose VM has ended.

Choose Lua multiplayer when the feature is expressed in game scripts and its payload fits the supported
Lua values. Use the [C++ multiplayer API](multiplayer.md) when a C++ patch needs to define its own byte
protocol. Scripts using this helper share `FC.LuaMessages`; each mod distinguishes its own messages with channels.

## Example

This ping test uses a playing host and a remote client. Load the following script once in the receiving
VM on each process after `FusionCutter.Multiplayer` is available. On the client, call `ExamplePing.Update(dt)`
from an existing game update callback; `dt` is elapsed seconds. The host replies when a ping arrives.

```lua
local mp = FusionCutter.Multiplayer
local elapsed = 0
local finished = false
ExamplePing = {}

local ok, message = mp.Listen("Example.Ping", function(sender, value)
    if mp.IsHost() and value == "ping" then
        local result = mp.SendClient(sender, "Example.Ping", "pong")
        if result ~= "accepted" then
            print("Ping reply was not queued:", result)
        end
    elseif value == "pong" and sender == mp.Host() and not finished then
        finished = true
        print("Host replied")
    end
end)
if not ok then
    error(message)
end

-- Try at most once per second until a reply arrives.
function ExamplePing.Update(dt)
    if finished or not mp.IsClient() then
        return
    end
    elapsed = elapsed + dt
    if elapsed < 1 then
        return
    end
    elapsed = 0

    local host = mp.Host()
    if not host or not mp.Ready(host, "Example.Ping") then
        return
    end
    local result = mp.SendHost("Example.Ping", "ping")
    if result ~= "accepted" then
        print("Ping was not queued:", result)
    end
end

-- Stop this test and release its listener while the VM remains alive.
function ExamplePing.Close()
    finished = true
    local closed, message = mp.Close("Example.Ping")
    if not closed then
        print(message)
    end
end
```

`ExamplePing.Update` and `ExamplePing.Close` are functions defined by this example. FC does not call them
automatically. For a shell mod, the following connects the test to the multiplayer lobby's existing update
callback. Run it once in the same VM, after `ifs_mp_lobby` is defined and the script above has run:

```lua
local update = ifs_mp_lobby.Update
ifs_mp_lobby.Update = function(this, dt, ...)
    ExamplePing.Update(dt)
    return update(this, dt, unpack(arg, 1, arg.n))
end
```

Keep the lobby open on the client while testing. `Update` waits for the host and its listener to become
ready, then sends a ping. If readiness is still pending or a send is refused, a later update tries again.
The listener is registered once; retries send messages rather than replacing that listener.

The host checks the reply's send result too. Its first reply can be refused if it has not yet received the
client's listener announcement. The next ping gives it another opportunity to reply. Once the client receives
`pong` from its current host, it prints `Host replied` and stops sending. The test deliberately repeats a
harmless request until it receives that response. For messages that change game state, retry refused sends;
resending an accepted message requires a protocol that handles duplicates.

Call `ExamplePing.Close()` when ending the test or leaving the session. VM destruction also removes its
listener. Run the first script again for another session, but install the lobby update wrapper only
once per VM. As with other mod scripts, arrange for the game's script loading to execute it; installing FC
alone does not load this example.

This example starts requests only on a remote client. A playing host also has a local client that can use
`SendHost`, but `IsClient()` is false there because its mode is Host. A dedicated host can run the listener
and reply through `SendClient`; it has no local client and cannot initiate `SendHost`.

## Script functions

| Function | Return and behavior |
| --- | --- |
| `Listen(channel, function)` | true, or nil and an error message; callback receives `(sender, value)` |
| `Close(channel)` | true, or nil and a message; removes this VM's listener; an absent listener succeeds |
| `SendHost(channel, value)` | A result string; clients and a playing host's local client can send to the host |
| `SendClient(peer, channel, value)` | A result string; host sends to a current client |
| `Broadcast(channel, value)` | Boolean acceptance summary; host sends to ready clients, including its local client |
| `Host()` | Host peer, or nil when absent; nil and a message when game context is unavailable |
| `Clients()` | One-based table of host's clients; empty on a remote client; nil and message without context |
| `IsHost()` | True for Host or Dedicated; nil and message without game context |
| `IsClient()` | True for Client mode; nil and message without game context |
| `Ready(peer, channel)` | True when the peer is compatible and has advertised that listener |

Peer values are opaque userdata. Pass them back to these functions and compare identities; do not convert
them to numbers or treat them as player indices. They expire with their connections. Invalid peer arguments
and invalid channel arguments to sends or Ready raise a Lua error. Listen and Close report registration
errors as nil plus a message.

One live VM owns a channel's listener in each process. Listening again from that VM replaces its handler;
another live VM cannot claim it. Closing or replacing a listener, ending its VM or losing the connection
cancels obsolete deliveries. Messages are delivered on the listener's Lua thread during game updates.
A script error in a listener is logged.

## Send results

| String | Meaning |
| --- | --- |
| `accepted` | Complete encoded message accepted by the runtime |
| `unavailable` | No applicable session, peer or direction |
| `pending` | Feature agreement still in progress |
| `incompatible` | Peer lacks compatible Lua messaging |
| `no_listener` | Peer has not advertised this channel |
| `full` | Queue cannot accept the message now |
| `too_large` | Encoded value or listener metadata exceeds the format or payload limit |
| `invalid_value` | Value cannot be serialized |
| `failed` | Feature or connection failed |

Acceptance does not confirm remote execution. Broadcast skips clients that are not ready, returns false
for an invalid host context or encoding failure, and returns false if any attempted send was refused.
Earlier accepted sends remain queued. With a valid host context and no ready recipients, it returns true.

## Values and limits

Serialization converts a Lua value into bytes for another process, then reconstructs a value in the
receiving VM. Sending a table therefore sends its contents, not a shared reference. Changing the original
table afterward does not change what the recipient received. A later change needs another message.

Messages support nil, booleans, finite numbers, counted strings and plain tables. Table keys may be booleans,
numbers or strings. Functions, userdata, threads, metatables and cyclic structures are unsupported. Shared
subtables are copied independently, so object identity is not preserved.

Numbers must be exactly representable by the receiving game's float32 Lua type. Strings preserve embedded
NUL bytes. The current format allows up to 16 nested tables and uses 16-bit string lengths and table-entry
counts; the feature payload limit can be smaller. The complete listener announcement must also fit the
feature payload. Listen rejects a change that would exceed that budget.

Reliable delivery also needs space while waiting for a Lua update. If a current accepted message cannot be
retained or scheduled, Lua messaging for that peer fails and reports the reason instead of silently
continuing as if delivery succeeded.
