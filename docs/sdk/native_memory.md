# Game memory and edits

For a complete example using real addresses, follow [your first game patch](game_patch.md). This guide
explains the memory operations available for that patch and more complex changes.

The game's memory contains its executing code and the data that code uses. A patch might replace a fixed limit,
redirect a pointer to a larger table, or change a machine instruction. The planning API describes
those edits so FC can check their locations, detect conflicts and own the resources they require.

Use these operations when the behavior you need depends on a known game location. If an existing service
already supplies the information, such as [game context](game.md), use that service instead of finding and
reading the same game state yourself. Calling or intercepting a function is covered in
[game functions and hooks](native_hooks.md).

Include `<fc/native.hpp>`. Submit memory operations in `Handler::plan`. The runtime copies their descriptions,
checks compatibility, prepares resources and applies changes during installation.

An operation's failure marks the shared plan as failed. For author decisions that cannot be expressed as an
operation, call `plan.fail(message)`. Continuing to submit operations does not clear the failure.

## Locations and evidence

An address identifies a byte in the current process. An **RVA**, or relative virtual address, identifies its
offset from the start of one loaded image. FC combines the selected image's actual base address with that
offset. For illustration, a base of `0x400000` plus RVA `0x1234` gives address `0x401234`; these numbers describe
the arithmetic, not a Battlefront patch site.

Windows can load an image at another base. That changes its absolute addresses while its RVAs remain useful
for the same build. An RVA is also different from an offset into the executable file: Windows maps file
sections into memory, and their file and memory positions need not match.

You obtain a location's RVA, type and expected contents by inspecting the supported binary. FC's catalog
identifies the image build; it does not discover the locations for a new patch. A **layout** is the author's
collection of these locations for a build, usually kept together so the patch's behavior can reuse them.

Bare RVAs use the plan's bound image. A one-image variant binds automatically. With several requirements,
use `auto game = plan.for_image("game")`; `game.profile()` identifies the selected build.

| Plan view method | Return and failure behavior |
| --- | --- |
| `for_image(name)` | A copied Plan bound to that requirement label; matching is ASCII case-insensitive. An unknown label fails the shared plan |
| `profile()` | Borrowed `string_view` of the bound profile ID; an unbound view fails the plan and returns an empty string |
| `at(rva, name = {})` | An owning Location in the bound image; an unbound view fails the plan and returns an invalid Location |
| `fail(message)` | Record the first planning error; subsequent submissions cannot recover the plan |
| `log()` | Logger attributed to this patch |

`for_image` leaves the original view's binding unchanged. All views submit to the same plan and share its
failure state. A profile string view expires with its Plan view; copy the string if it is needed later.

| Location type | Meaning |
| --- | --- |
| `Location` | Selected profile ID in `image`, byte `rva`, optional diagnostic `name`, and derivation failure flag |
| `DataLocation<T, Count = 1>` | Typed data at an RVA, with `name` and `evidence`; exposes `value_type = T` and constant `count = Count`, which must be positive |
| `CodeLocation` | Instruction RVA, name and evidence |
| `FunctionLocation<Call>` | Function entry with its call signature |
| `CallLocation<Call>` | A direct CALL instruction with its callee signature |
| `VtableLocation` | A CodeLocation used as the start of a function-pointer table |

`Location::image` is a selected profile ID, not a requirement label. Typed locations contain an RVA and use
the Plan view's image. Labels describe purpose within a variant; profiles identify the actual selected build.

For an operation between images, construct each Location from the appropriate view. In this example,
`pointer_rva` is a pointer field in the game and `function_rva` is a function in the supporting `peer` image:

```cpp
auto game = plan.for_image("game");
auto peer = plan.for_image("peer");
game.write_pointer(game.at(pointer_rva), fc::address(peer.at(function_rva), function_evidence), pointer_evidence);
```

The enclosing variant must declare both labels. Supply the RVAs and evidence from verified layouts;
source and destination evidence are checked against their respective images.

Evidence owns its bytes and remains valid when constructed from temporaries:

| Helper | Check |
| --- | --- |
| `exact_bytes(bytes)` | Every supplied byte matches; accepts byte spans or a list of `uint8_t` values |
| `masked_bytes(bytes, mask)` | Compare bits selected by each corresponding mask byte |
| `expect(value)` | Compare a trivially copyable value's complete representation, including padding |
| `points_to(location)` | A pointer-sized field contains that image address |
| `direct_call_to(location)` | A five-byte direct relative CALL targets that location |
| `direct_jump_to(location)` | A five-byte direct unconditional JMP targets that location |

Supply evidence for the assumptions a patch depends on. Prefer target evidence for relocated pointers and
branches; hard-coded absolute addresses from a disassembler can change when Windows loads an image elsewhere.
Masked evidence must still check the meaningful instruction bytes. Empty evidence does not establish an
expected original value, though the runtime still checks the operation's range, access and instruction rules.

`exact_bytes` requires a nonempty input; `masked_bytes` requires nonempty, equally sized byte and mask inputs.
Both helpers copy `uint8_t` spans, `Bytes` or initializer lists.
`Location::view()`, `Evidence::view()` and `Address::view()` expose C records for adapters; their pointers
borrow the source object's storage. Ordinary patches pass the C++ objects directly to Plan.

Evidence answers whether the particular assumption behind an operation is still true. For example, a patch
expecting a limit of 16 can declare that value before replacing it. If another modification has changed the
limit, FC can report the mismatch instead of silently applying the replacement to unexpected state.

Choose evidence according to the assumption. Use `expect` for a known stored value and `exact_bytes` for
known instruction bytes. If a field points to another image location, use `points_to` so the check accounts
for the loaded base. A mask is useful for bytes that legitimately vary, but masking the very operand the
patch depends on would remove that check.

## Choose when the memory is used

| Need | Operation | Why |
| --- | --- | --- |
| Verify expected bytes without using their value | `Plan::check` | Records a compatibility requirement and fails the plan if it does not hold |
| Inspect a value while building the plan | `Plan::read` | Returns a snapshot for a startup decision |
| Install a fixed replacement | `Plan::write` | Queues the edit for runtime installation |
| Create storage the game will use | `Plan::allocate` | Reserves stable storage owned with the installed patch |
| Read or change a reserved region during gameplay | `Plan::access`, then `Memory` | Retains access after the Plan call has ended |
| Read a field of a valid object received by a hook | `read_field` | Copies at a known object offset without nested pointer casts |

A pointer received during gameplay is not the same as a fixed image location. For example, a player object
may be destroyed and replaced between missions. A memory handle or field helper does not extend that object's
lifetime; the patch must use it only while the game still owns a valid object.

## Planning reads and writes

Use `check` when the patch depends on bytes at a location but does not need to use their value:

```cpp
void check_instruction(fc::Plan &plan, const fc::CodeLocation &site) {
    plan.check(site, 3);
}
```

This example checks a three-byte instruction using the site's evidence from the supported build's layout.
`check` returns `void`; a failed read or evidence mismatch records a plan failure automatically. The runtime
reserves the checked range against conflicting writes and rechecks the evidence before installation. It uses
the same validation as `read_bytes`. Use `read` or `read_bytes` when you also need the returned value.

```cpp
#include <fc/native.hpp>

// Copies a verified limit into the plan, then queues its replacement for installation.
void replace_limit(fc::Plan &plan, const fc::DataLocation<std::uint32_t> &site, std::uint32_t replacement) {
    auto original = plan.read(site);
    if (!original) {
        plan.fail(original.error().message);
        return;
    }
    plan.log().debug("Changing limit from {} to {}", *original, replacement);
    plan.write(site, replacement);
}
```

The caller supplies `site` from a layout verified for the selected profile. A planning read copies the
value now; it is not a live view. A planned write applies later.

In `replace_limit`, the read lets the patch describe the change in a Debug message. It also returns an error
if the site cannot be read as declared. The write copies `replacement` into the plan, so the local function
argument need not survive installation. If all you need is a fixed replacement and the site's evidence
already describes the expected original value, a write alone is sufficient.

| Plan method | Result and behavior |
| --- | --- |
| `check(location, count, evidence)` | Verify a nonempty byte range without returning its contents; failure rejects the plan |
| `check(CodeLocation, count)` | Verify an explicit byte count using the site's evidence |
| `read<T>(location, evidence)` | `Result<T>`; snapshot of a trivially copyable value |
| `read(DataLocation<T, N>)` | `Result<T>` for N = 1, otherwise `Result<std::array<T, N>>` |
| `read_array<T>(location, count, evidence)` | `Result<std::vector<T>>`; count must be nonzero |
| `read_bytes(location, count, evidence)` | Byte-vector snapshot |
| `write(location, value, evidence)` | Copy a trivially copyable value into the plan; destination needs `alignof(T)` |
| `write(DataLocation<T, N>, value)` | Write the typed value or fixed array with the site's evidence |
| `write(location, span, evidence)` | Copy the span's elements, preserving their alignment requirement |
| `write(location, Bytes, evidence)` | Copy raw bytes with byte alignment |
| `write(CodeLocation, Bytes)` | Replace complete instruction bytes using the site's evidence |

`check`, the location-based read methods and ordinary typed writes also accept a bare RVA. `Bytes` is
`std::span<const std::byte>`; use it when you mean raw replacement bytes. Writing a pointer as an ordinary
value writes that pointer's representation; use the address operations below for planned address resolution.

Counts are element counts for `read_array<T>` and byte counts for `check` and `read_bytes`; all require a
nonempty range. Typed reads require `alignof(T)` at the source. Failures include an invalid range, alignment,
access, evidence or a conflict with another operation, and leave the plan failed even if a returned Result
is ignored. Successful snapshots own their copied values and may be retained after Plan.

## Allocate replacement data

Allocation is useful when a feature needs more storage than the original game provides. A larger table
needs both stable storage and edits that make the game use it. A temporary local array would disappear when
Plan returns; a runtime allocation remains available for installed uses.

`allocate<T>(count, placement)` reserves zero-filled storage. Overloads accept a `span<const T>`, `std::array`
or C array and copy initial values. T must be trivially copyable.

`Placement` contains `alignment` (zero uses `alignof(T)`) and optional `near_image`, a selected profile ID
requesting storage within relative-address reach. Explicit alignment must be a power of two.

Placement defaults to `{}`. Allocation counts must be nonzero. The returned Data handle describes a
reservation, so obtaining it does not mean memory is already available. Allocation or placement failure
rejects the patch before installation; use `values()` during Prepare for initialization.

```cpp
void plan(fc::Plan &plan) {
    replacement_ = plan.allocate<std::uint32_t>(16);
    live_ = plan.access<std::uint32_t>(replacement_);
}

fc::Result<> prepare(fc::PrepareContext &) {
    auto values = replacement_.values();
    if (values.size() != replacement_.size()) {
        return std::unexpected(fc::Error{"Replacement table is unavailable"});
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<std::uint32_t>(i);
    }
    return {};
}
```

Here the handler retains `fc::Data<std::uint32_t> replacement_` and `fc::Memory<std::uint32_t> live_`.

Plan reserves 16 entries and a handle for later access. Prepare obtains the initialization span and fills
those entries before installation. The example only prepares storage; to make the game use it, the actual
patch must also submit the verified pointer or operand edits referring to `replacement_.address()`. Any count
or bounds in the game's code must agree with the replacement table's size.

`Data<T>` describes the allocation and its initialization. `Memory<T>` provides continuing access after
installation. If the data will never change after Prepare, retain only the allocation handle you need.
The element type must be trivially copyable, meaning its value can be copied as bytes; a `std::string` or
`std::vector` object is not a replacement for the game's own binary data layout.

| `Data<T>` method | Meaning |
| --- | --- |
| `size()` | Declared element count, available before preparation |
| `address(index = 0, member = 0)` | Planned address of an element plus a byte offset within T |
| `values()` | Mutable initialization span during Prepare; empty when unavailable |
| `resource()` | Opaque runtime identity, mainly for adapters |

`address(size())` permits a one-past-end address with no member offset. The runtime owns the allocation.
Other indices must be within the allocation, and `member` must be less than `sizeof(T)`. An invalid address
description fails the plan when submitted.

Stop using the initialization span to mutate data once installation publishes it. Request `Memory<T>` for
continuing access, and synchronize access with the game.

## Continuing memory access

`access<T>(location, count = 1, evidence)` returns `Memory<T>`. It also accepts a bare RVA, a typed
`DataLocation`, or this plan's `Data<remove_const_t<T>>`. `const T` requests read-only access; mutable T
requests read/write access. The handle becomes usable after resource preparation.

| `Memory<T>` method | Meaning |
| --- | --- |
| `read(index = 0)` | Copy one element into `Result<remove_const_t<T>>` |
| `write(value, index = 0)` | `Result<>`; write one element; available only for mutable T |
| `copy_to(span)` | `Result<>`; copy the leading elements out |
| `copy_from(span<const T>)` | `Result<>`; copy into the leading elements; mutable T only |
| `view()` | Return `Result<std::span<T>>` after checking current access |

Copy operations check the declared range and current access. Empty span copies succeed. A returned span
refers to storage owned by the game or runtime: later access, object lifetime and synchronization are still
the author's responsibility. Memory access errors return `Error`; they do not make arbitrary game pointers safe forever.

Default-constructed Data and Memory handles refer to no allocation or access reservation. Data reports zero
size and an empty initialization span. Nonempty Memory operations return errors until a prepared resource
is assigned; empty span copies succeed without accessing storage.

For a pointer or integer address already valid in a hook callback, `read_field<T>(object, byte_offset)` and
`write_field(object, byte_offset, value)` perform unaligned copies with `memcpy`. They accept pointers or
`uintptr_t`, avoiding nested casts. They do not validate the address. Use them only with established object
layouts and valid storage, independently of the planned `Memory` interface.

Field offsets are signed byte offsets. `read_field<T>(object)` defaults to offset zero and returns T
directly; `write_field(object, offset, value)` returns void. Neither operation returns `Result` or catches
an access violation.

## Addresses, operands and branches

Machine instructions can contain a literal value, a displacement or a branch destination. These are
**operands**. When only an encoded operand needs changing, `operand` expresses that intent without replacing
the rest of the instruction. A NOP is an instruction that does no work, so `nop` removes the effect of a
verified instruction sequence while preserving its occupied space.

CALL and JMP are control-flow instructions. A call transfers execution and supplies a return address;
a jump transfers execution without that call/return step. Redirect operations change an existing branch's
destination. Insert operations create a branch in a verified span of instructions. Choose a
[typed hook](native_hooks.md) when you need a C++ callback with the game function's arguments and an
original-call handle.

`Address` describes a destination in a selected image, a plan allocation, or plugin code:

- `address(location, evidence)` checks an image destination.
- `data.address(index, member)` resolves an allocation during preparation.
- `address(function_pointer)` identifies executable code in the submitting plugin.

| Plan method | Effect |
| --- | --- |
| `write_pointer(location, address, evidence)` | Write a pointer-sized absolute address |
| `write(location, rel32(address), evidence)` | Write a signed four-byte displacement measured from the field's end |
| `operand(location, index, immediate(value), evidence)` | Replace a zero-based explicit operand's encoded immediate/displacement, preserving its width |
| `operand(location, index, address, evidence)` | Replace a decoded operand with an address; derive relative displacement where required |
| `nop(location, byte_count, evidence)` | Replace exactly that many complete instruction bytes with NOPs |
| `redirect_call(location, address, evidence)` | Retarget an existing five-byte direct CALL |
| `redirect_jump(location, address, evidence)` | Retarget an existing five-byte direct JMP |
| `insert_call(location, byte_count, address, evidence)` | Insert a CALL over at least five complete instruction bytes, filling the rest with NOPs |
| `insert_jump(location, byte_count, address, evidence)` | Insert a JMP with the same extent rules |

These operations also have bare-RVA forms, except the `rel32` write, which uses a Location. `nop` and both
`operand` forms accept a CodeLocation and use its evidence. `immediate` preserves signedness so values are
checked against the actual encoding. Address writes check reach and storage; source evidence and destination
evidence describe separate assumptions.

`immediate(value)` constructs `Immediate<T>` for an integer value; `rel32(address)` constructs RelativeAddress.
These wrappers select how an operand or field is encoded. All untyped edit and branch methods above return
void and record failure in the plan.

An Address can describe an allocation before it has a usable process address. FC resolves it when preparing
the plan. This is why a planned pointer write takes Address rather than requiring the author to allocate
memory early and convert it into a raw pointer.

A `rel32` field stores a signed distance from the end of the field to the destination. It can represent only
destinations within that distance's range. `near_image` placement is useful when an allocation will be reached
through such a field. Ordinary allocations need no special placement just because the patch uses a game image.

Typed redirect overloads accept a `FunctionLocation<Call>` or matching compiler function pointer and return
a `NativeFunction<Call>` for the original destination. Sources can be Location, CodeLocation or RVA;
`redirect_call` also accepts a `CallLocation<Call>`. See [game function calls](native_hooks.md).

## Location and byte helpers

| Helper | Result |
| --- | --- |
| `element(DataLocation<T, N>, index)` | One typed array element |
| `offset(DataLocation<T, N>, signed_bytes)` | Shift a typed location by bytes |
| `vtable_slot<FunctionPointer>(VtableLocation, index)` | One function-pointer slot |
| `byte_array<0x90, 0x90>()` | Fixed `std::array<std::byte, N>` |
| `write_value<Offset>(array, value)` | Copy a trivially copyable value at a compile-time byte offset |
| `relative_displacement<SignedType>(base, target)` | Optional signed `target - base`; use the instruction/field end as base |

Derived typed locations clear the parent's evidence; attach evidence for the derived site. Bounds and RVA
overflow produce an invalid location that rejects later submission. `write_value` checks array bounds at
compile time. These helpers construct descriptions and bytes; they do not install changes themselves.
