# Game context

Game context tells a patch what kind of session this process is participating in and which session it is.
Use it to run host-only behavior, suspend a feature at the menu, or clear state left over from a previous
mission or connection. The runtime reads the supported game's state and presents the same small interface
to every patch that needs it.

For example, a patch maintaining session statistics needs to know when a new session begins. Reading
`GameState::session` lets it associate its counters with that session instead of treating the entire game
process as one continuous match. A patch providing host behavior can check the current mode without copying
the game's internal logic for detecting the host.

Include `<fc/game.hpp>`. Retrieve a `Game` handle with `PrepareContext::game()` and retain it as needed.
It reports actual session participation, independently of the loader's Client or Server role.

## Launch role and current mode

The launch role describes how the process was started and is fixed for that FC run. A normal game launch
has Client role even if the player later hosts a multiplayer session. Its current GameMode can move from
None at the menu to Local for singleplayer, Host for a hosted match or Client for joining another host.
A dedicated launch has Server role; its active hosting mode is Dedicated.

Use a [variant role](declarations.md#launch-roles-and-variant-overrides) for installation eligibility. Use
GameMode for behavior that can change while the process remains open. `is_host()` includes Host and Dedicated;
it does not include Local. A feature that should also operate in singleplayer must explicitly include Local.

## React to a session change

Register once during Prepare. The callback receives the latest state after activation and when it changes:

```cpp
fc::Result<> prepare(fc::PrepareContext &context) {
    game_ = context.game();
    return game_.on_change([this](std::optional<fc::GameState> state) noexcept {
        state_ = state;
    });
}
```

Here `game_` is `fc::Game` and `state_` is `std::optional<fc::GameState>`, read only from other control-thread
callbacks. Use `game_.current()` for a fresh snapshot on another thread instead of sharing that variable
without synchronization.

The example stores a snapshot for later Update or Status calls. A statistics patch could also compare the
new session ID with its previously stored ID and clear its counters when they differ. A feature
that only needs a decision at the moment of a hook can call `current()` there instead of storing a second copy.

An unavailable snapshot is different from a supported game reporting None. For example, a status patch can
display that game context is unavailable on an unsupported build, while displaying that no session is active
for None. Treating both as singleplayer would make a feature run on assumptions FC has not established.

## Methods and values

| API | Meaning |
| --- | --- |
| `Game::current()` | Nonblocking snapshot on any thread; `nullopt` means unavailable |
| `Game::on_change(callback)` | Register during Prepare; callback takes `optional<GameState>` and must be `noexcept` |
| `GameState::session` | Process-local monotonic `SessionId` (`uint64_t`); zero means no active session |
| `GameState::mode` | Current GameMode |
| `GameState::is_host()` | True for Host or Dedicated |
| `GameState::is_client()` | True only for Client |

`on_change` returns `Result<>`. Registration can fail if notification access is unavailable or the call
is outside Prepare. Successful registration does not imply known game state: its callback may receive
`nullopt`. Both a default-constructed Game handle and a runtime without current state return `nullopt`
from `current()`.

| `GameMode` | Meaning |
| --- | --- |
| `Local` | Local singleplayer mission |
| `Host` | Hosting multiplayer with a local player |
| `Client` | Joining or playing on another host |
| `Dedicated` | Hosting multiplayer without a local player |
| `None` | No active session; session ID is zero |

The first change callback follows patch activation, then the control thread delivers changes. Rapid changes
may be combined into the latest state. A copied GameState does not stay current, and this interface is not
an event log of every game transition. Session IDs identify a local mission run or multiplayer attempt;
they are neither player IDs nor values to exchange with another process.

`nullopt` and `GameMode::None` differ: the first means FC cannot supply state, the second is known idle state.
Builds without an integration remain unavailable. See the Game context row in the
[build support tables](../installation.md#feature-support), including the restriction to local missions in ModTools.
LAN hosting uses Host or Dedicated where the build's game context implementation supports it; that does not
imply Galaxy messaging or feature negotiation is available over LAN.

Handle availability in your feature and select compatible builds in the declaration. Game context is a
runtime service and needs no helper-patch dependency.
