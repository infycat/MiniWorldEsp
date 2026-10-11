# Source notes

## Verified live creature source (2026-10-11)

This finding supersedes the earlier notes below that said the live C++ reader
could not enumerate creatures and had to rely on the Lua Area query. CE MCP
disassembly of `libSandboxEngine.ClientActorMgr::tick` confirmed that
`tick + 0x190` (`libSandboxEngine.dll + 0x5B4C60` in the inspected build)
executes `mov ecx,[edx+edi*4]`, skips null entries, and dispatches the actor's
virtual tick. The loop reads a contiguous vector of 32-bit actor pointers from
globals `libSandboxEngine.dll + 0x25CB430` (begin) and `+0x25CB434` (end).

The live vector contained runtime labels `ClientMobNN`, `ActorVillagerNN`,
`ActorHorse`, and `ClientItem`. Its entries exposed valid actor position
components at `actor + 0x270`, with XYZ integer coordinates at
`component + 0xDC`. The C++ reader now samples this vector read-only, rechecks
begin/end after copying to reject concurrent reallocations, and deduplicates
against the World ActorManager list. It classifies `ClientPlayerNN` as players
and verified `ClientMobNN`, `ActorVillagerNN`, animal, monster, or mob labels as
creatures; items and unknown labels remain separate. Creature boxes use the
documented generic Lua fallback until per-class native bounds are verified.
The overlay and rate-limited log expose vector bounds, slot count, accepted
count, and validity. The older Area-query and WID investigation below remains
background research, but is no longer a blocker for the live C++ creature path.

## CE-verified health and skeleton accessors (2026-10-11)

CE MCP symbol resolution and disassembly for the attached 32-bit
`MiniGameApp.exe` confirmed these accessors:

- `libSandboxEngine.ClientActor::getAttrib` at `0x608D1240` returns the
  pointer at `actor + 0x280`.
- `libSandboxEngine.ActorAttrib::getHP` at `0x606C72B0` loads a float from
  `attrib + 0x94`; `ActorAttrib::getMaxHP` at `0x608DAA00` loads a float from
  `attrib + 0x9C`.
- The live reader now follows those verified fields with read-only
  `ReadProcessMemory`, validates finite/nonnegative values, and supplies
  optional health to the existing health-bar/number renderer.
- `libSandboxEngine.ClientActor::getBody` at `0x606C8D80` returns
  `actor + 0x194`. `libSandboxEngine.ActorBody::getBindPointPos` at
  `0x60948030` is the source accessor used by the Lua skeleton renderer.
- `ActorBody::getBindPointPos` resolves a handle at `body + 0x88`, caches a
  resolved entity at `body + 0x8C`, then invokes the entity vtable slot `+0x80`
  to obtain the requested bind-point world matrix. The matrix path continues
  through `libMiniBaseEngine.Rainbow::Entity::GetAnchorWorldMatrix`
  (`0x5E376E80`), `FindAnchorOwnerModel` (`0x5E376C20`), and
  `Rainbow::MovableObject::GetWorldMatrix` (`0x5E3F0F10`). This is a live
  method/vtable path, not a flat array of bone coordinates; C++ external
  `ReadProcessMemory` cannot call it. The C++ renderer therefore still has no
  real per-frame bind-point positions or skeleton links. Do not fabricate them
  or invoke target functions from the external overlay.

This folder is the standalone C++ project. It does not depend on files outside
this directory.

- The camera projection and matrix conversion are implemented in
  `src/esp/projection.cpp` and `src/esp/projection.hpp`.
- The DirectX 11 window and ImGui controls are implemented in
  `src/esp/overlay_app.cpp` and `src/esp/overlay_app.hpp`.
- The UI merges installed Windows fonts for Latin extensions, Thai, Indic
  scripts, symbols, and CJK names. The underlying player-name bytes are kept as
  UTF-8; no locale-dependent ANSI conversion is applied.
- ESP and skeleton rendering are implemented in `src/esp/draw_esp.cpp`.
- Snapshot parsing and background updates are implemented in
  `src/esp/data_source.cpp`.
- `ce_data_source.cpp` from the supplied reference project is intentionally not
  copied into the build. It reads guessed addresses and fabricates entities and
  bones, so it cannot provide trustworthy live game data.

The live reader uses read-only `ReadProcessMemory` against the current
version-matched layout. CE MCP inspection confirmed the `PlayerControl` camera
chain used by `PlayerControl::getPointToScreen`: controller `+0x950`, render
camera `GameCamera+0x60`, with view/projection matrices at render camera
`+0x44/+0x84`. The renderer camera is cross-checked against `WorldRenderer+0x98`
before use. Actor positions are verified from the ActorManager pointer span.
CE MCP also confirmed the current `ClientPlayerN` type label at actor `+0xd0`
and disassembled `ClientPlayer::getNickName`, which reads the string object at
`+0x578` (length `+0x588`, capacity `+0x58c`). The C++ reader decodes both
small-string and heap-string cases on each poll, so player names are refreshed
with their positions. Player actors and other validated ActorManager entries
now use a generic Lua fallback 1 m by 2 m box, offset from the source base
position to the renderer's center position. Team IDs are not read by this
source, so remote players remain visible as unclassified players rather than
being dropped by a friend/enemy filter. `ClientActor::getActorName` disassembly
also shows an optional C-string
pointer at actor `+0x210`; the reader copies that string for non-player actors
when present. In the inspected map, the ActorManager span contains players only;
`mianbao.lua` gets creatures from the separate `Area:getAllObjsInAreaRangByTypes`
query (type `1`). The C++ source does not call that game-native query, so it
cannot yet produce live creature entries; it does not invent creature
categories, names, or boxes. The module base is refreshed from the current
process module list on every poll, while the global controller is addressed as
`libSandboxEngine.dll + 0x027772F8`. A failed snapshot clears the displayed
world immediately so room-exit or process-loss frames do not linger.

The exact Lua-side creature enumeration function is
`class.Area.template:getAllObjsInAreaRangByTypes(posbeg, posend, {1})`.
This is only a Lua wrapper: it turns type indices into a bit mask, adjusts the
range, and calls `SSMgrArea:LockAllObjsInAreaRange_Types(...)`, followed by
`getLockedObjInAreaRange(i)` and `UnlockAllObjsInAreaRange()`. The source for
`SSMgrClassArea:Instance()` shows that unresolved methods are forwarded to
`CurWorld:getSceneAreaMgr()`. `WorldGen:getSceneAreaMgr()` lazily creates that
object with `SceneAreaManager:create()`, proving the query implementation is
provided by a native C++-bound `SceneAreaManager`, not by the Lua wrapper.

In the currently attached build, the `libiworld.dll` binding table points the
three methods to Lua/C++ adapter thunks at module RVAs `+0x425480`
(`LockAllObjsInAreaRange_Types`), `+0x425150` (`getLockedObjInAreaRange`), and
`+0x4259C0` (`UnlockAllObjsInAreaRange`). CE MCP disassembly confirms the lock
adapter reads seven numeric arguments and dispatches through a callable
adapter; the getter reads a vector stored at manager offsets `+0x28/+0x2C`,
with 8-byte entries; unlock resets that vector's end to its begin. The adapter
dispatches through an object vtable at slot `+0x10`, so these thunk RVAs are
not safe substitutes for the per-instance C++ method pointer. The live C++
reader still needs the actual `SceneAreaManager` instance and object-ID-to-actor
resolution path before it can emit live creatures. Neither a synthetic
creature nor a guessed native call is used as a fallback.

Further CE/source tracing confirms the IDs in the area result vector are
64-bit WIDs, not 32-bit pointers: the Lua API maps player/creature/drop/missile
to area type indices `0/1/2/3`, and `getAllObjsInAreaRangByTypes` converts each
index to a bit in the native type mask. The getter's index calculation is
`(end - begin) >> 3`, and it loads both dwords from `begin + index * 8`, which
preserves the full WID. The Lua `Mob:getActorByObjid` path resolves these WIDs
through `CurWorld:getActorMgr():findActorByWID`; the more specific mob path uses
`findMobByWID`, while `Actor:isMob` treats IDs above `2^32` as mob WIDs. The
live C++ ActorManager span currently inspected contains `ClientPlayerN`
objects only, so it is not a complete actor list and cannot supply creatures
by itself. The remaining native work is to locate the live area-manager
instance/result storage and the ActorManager's WID-to-Mob lookup table; until
those are verified, do not cast area WIDs to pointers or emit guessed mob
coordinates.

The Lua-side classification path is more specific than the area type mask:
`services/actor.lua` implements `SSActor:isMob(objid)` by calling
`CurWorld:getActorMgr():findMobByWID(objid)` for IDs above `2^32`, while its
`getPosition` path resolves the actor and calls `getPosition(0, 0, 0)` (engine
coordinates, then scales to meters). `services/mob.lua` exposes the same
WID-to-actor path, and `services/area.lua` uses `findActorByWID` after an area
query. Therefore, area type `1` alone is not a sufficient classification
signal: validate the WID with `findMobByWID` (or the source-equivalent mob
lookup) before labeling it a creature. This avoids promoting arbitrary
area-query results or non-mob objects into the creature ESP.

The `WorldGen:getSceneAreaMgr()` source stores its lazily-created native
userdata in the Lua field `m_pSceneAreaMgr`; it is not a field in the verified
native `World + 0x164` ActorManager pointer chain. The C++ reader is external
and read-only, so it cannot safely invoke the Lua wrapper or its native
virtual methods. Direct `run_script` calls through the local Lua MCP returned
the previous probe log rather than a unique marker from the submitted snippet;
those output lines are stale and are not accepted as a fresh live creature
sample. Do not use them to tune C++ offsets.

Current implementation boundary: the live reader can enumerate and render the
verified player ActorManager vector. Creature integration remains pending
until a fresh, reproducible read-only route supplies both (1) the current
SceneAreaManager/result vector or an equivalent native mob-list pointer and
(2) the `findMobByWID` object resolution/layout. No creature records should be
fabricated in the meantime.
The pointer validator accepts the game's 32-bit large-address-aware heap range;
the former `0x7FFF0000` ceiling rejected live actor-vector and render-camera
allocations observed by CE MCP at `0x8A47AFC8` and `0x8A64C8D0`.

The demo snapshot is no longer the default. Start the synthetic scene with
`--demo`; it carries a `demo: true` flag and the overlay labels it permanently.
The live process source is the default; use `--demo` for the synthetic scene.
Live positions are engine coordinates at 100 source units per meter, while demo
snapshot coordinates are meters. Projection matrices are read from the same
render camera as the live scene data.

## Skeleton (bone) data contract

The ESP renderer draws a stick-figure skeleton for any entity that supplies bone
coordinates. The contract is data-driven and game-agnostic:

- `Entity.bones` — a map of `name -> {x, y, z}` world-space positions.
- `Entity.boneLinks` — an ordered list of `[from, to]` bone-name pairs. When
  omitted, the renderer falls back to the built-in humanoid chain
  (`defaultLinks()` in `draw_esp.cpp`): head→neck→chest→spine→pelvis, plus the
  four limbs.
- `Settings.showSkeleton` toggles the feature; `skeletonColor`,
  `skeletonOpacity` and `skeletonThickness` control its look.

Two ways to feed it:

1. **Offline / demo JSON.** `sample_data.json` already ships explicit `bones`
   and `bone_links` for every entity. `sample_data_no_bones.json` intentionally
   omits them; with `Settings.applyDefaultRigWhenMissing` on (the default) and a
   `demo: true` snapshot, the renderer synthesizes a generic biped rig from the
   entity position and half-height so the skeleton stays visible. This is a
   visualization aid only and never runs for live data (guarded by
   `world.demoData`). Try it with:
   `mcp_esp_overlay.exe --data sample_data_no_bones.json`
2. **External / live bones.** `JsonSnapshotSource` (`--data bones.json`) consumes
   any JSON in the `world_snapshot.schema.json` shape, so a bone snapshot can be
   produced by any tool and rendered as-is.

Live MiniGameApp bones: as documented above, `ActorBody::getBindPointPos` is a
vtable method reachable only in-process; external `ReadProcessMemory` cannot call
it, and this project does not fabricate bone positions. To obtain live bones,
export them from an in-process accessor (for example a Cheat Engine Lua script
that calls `getBindPointPos` and writes each bone's world matrix) into the JSON
contract above, then load it with `--data`. The renderer and the existing
`worldToScreen`/clip-space projection already support that path with no further
code changes.

Map `camera.forward` to `ClientPlayer:getCameraLookDir()` and `camera.position`
to the current eye position (`ClientPlayer:getEyePosition()`). The look
direction is a vector, not a view or projection matrix; the supplied view and
projection matrices must still match that camera and frame.
