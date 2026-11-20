# Out-of-bounds read in `PZBallisticsTarget::updateSkeletonBoneTransforms` (PZBullet64.dll)

> **Correction, 2026-11-20.** A live test on an unmodded 42.22.2 skeleton disproved the trigger described below. At runtime `Translation_Data` reaches native code with parent `0`, not `-1`, and vanilla characters pass 35 bones, so the vanilla walk stays in bounds. The unchecked parent index and the unchecked `numberOfBones` in the native loop are real, and the crash address is unchanged, but the input that triggers the crash is still being identified. Treat the "Root cause" and "Reproduce" sections as unverified until this notice is removed.

## Summary

- `PZBallisticsTarget::updateSkeletonBoneTransforms` reads `world[-1]` each time it walks a human skeleton. That happens whenever a firearm aim puts a character in the ballistics target list.
- The bone `Translation_Data` has no parent in the body mesh, so its parent index reaches native code as `-1`. The walk uses that index on the world-transform array without checking it.
- The read lands 0x80 bytes before a freshly allocated heap buffer. That memory is usually mapped, so the game reads one garbage transform and carries on.
- When the buffer starts a page and the page before it is unmapped, the read faults and the client closes to desktop at `PZBullet64.dll+0x269fd`.
- Seen on Project Zomboid 42.22.2, Windows client, `pzbullet=1.0.0.28`, revision `3d39148846`. The crashing client ran the vanilla 35-bone body mesh (no mod replaced `MaleBody.x` or `FemaleBody.x`).

| Library | SHA-256 |
| --- | --- |
| `PZBullet64.dll` | `76141df900f08ef71c0808bcd6248c929c47b8c5900b86af4875131a780897ef` |
| `libPZBullet64.so` | `b5d6178a789280c3bc4787ddf24f1bd44e4037d2d097bf6b4e5b4174c9b67354` |

## Where

| | |
| --- | --- |
| Function | `PZBallisticsTarget::updateSkeletonBoneTransforms(int numberOfBones, float* data)` |
| Windows | `PZBullet64.dll` RVA `0x26590`, faulting load at RVA `0x269fd` |
| Linux | `libPZBullet64.so` `0x172ef0` (unstripped, the source of the symbol name) |
| JNI entry | `Java_zombie_core_physics_Bullet_updateBallisticsTargetSkeleton` |
| Java callers | `BallisticsTarget.updateSkeleton()`, from `CombatManager.processBallisticsTargets` (aim) and `processInstantExplosion` |

Crash frame from the field: `# C [PZBullet64.dll+0x269fd]` on `MainThread`, about 0.15 s after the aim key went down.

## Root cause

The walk builds each bone's world transform from its parent's.

```cpp
world[0] = this->transform;
for (int i = 1; i < numberOfBones; i++)
    world[i] = world[ pose->skeleton->bones[i].parent ] * local[i];   // parent is never checked
```

Bone records are 0x20 bytes with the parent index at offset 0. `world` and `local` hold 0x80-byte `btTransform` records (double precision) and are allocated on each call, `35 * 0x80 = 0x1180` bytes each.

The `-1` comes from the normal content path.

1. `MaleBody.x` and `FemaleBody.x` declare `Translation_Data` as a top-level frame, a sibling of `Dummy01`.
2. `RagdollBuilder.getSkinningDataSkeletonHierarchy()` stores `boneHierarchy[Translation_Data] = parentBone.index()`. The parent resolves to `SkeletonBone.None`, and `None.index()` returns `-1`.
3. `Bullet.initializeRagdollSkeleton(SkeletonBone.count(), boneHierarchy)` passes the array to native `Skeleton::initialize`, which stores `-1` unchanged.
4. `Translation_Data` is bone 34, the last of 35. The loop reaches `i == 34` and loads `world[-1]`, which is `world - 0x80`.

Other native code treats `-1` as a sentinel. `RagdollBuilder::setupDefaulSkeleton` checks `parent != -1` before it touches the parent transform. This walk does not.

### Why it crashes at random, and only on Windows

The stray read hits whatever sits before the `world` buffer. In a packed heap that is another live allocation, so nothing faults. The read faults only when the allocator puts the buffer at the start of a committed region with an uncommitted page in front. The Windows heap produces that layout now and then, after minutes or hours of play. glibc's layout on Linux does not produce it in practice, so the defect is present there but silent.

### Related overrun in the same loop

The loop bound is the caller's `numberOfBones`, and nothing compares it with the native skeleton's 35-entry bone table. A body mesh with more than 35 bones would read `bones[i]` past the table. Vanilla bodies have 35 bones and only hit the `-1` case.

## Reproduce

### 1. Standalone harness (no game needed)

`repro.cpp` copies the walk and places `world[]` at the start of a page with an inaccessible guard page in front. The `world[-1]` load then faults on every run.

```
make run
```

Expected output on Linux (exit 139 is SIGSEGV):

```
world[] buffer at 0x....000, guard page immediately before it
walking 35 bones; bone 34 (Translation_Data) has parent -1
expect a fault reading world[-1] at 0x....f80 (world base - 0x80) ...
SIGSEGV reading 0x....f80  (inside world[-1], i.e. world base - 0x80)
exit: 139
```

On Windows, build with `cl /EHsc repro.cpp`. The guard page there is a reserved `PAGE_NOACCESS` page and the process dies with an access violation.

### 2. The shipped DLL, on demand

Full PageHeap with backward alignment puts a guard page before every heap block, so the same read should fault on the first aim.

1. Install Debugging Tools for Windows (`gflags.exe`).
2. Run `gflags /p /enable ProjectZomboid64.exe /full /backwards`.
3. Launch the game with no mods, load a character, and aim a firearm at a zombie.
4. Expect an access violation at `PZBullet64.dll+0x269fd`, reading just below a heap block.
5. Turn it off with `gflags /p /disable ProjectZomboid64.exe`.

Default PageHeap guards the end of each block and does not catch this read, so `/backwards` is required.

## Suggested fix

Treat a negative parent as the root inside the walk.

```cpp
int parent = bones[i].parent;
world[i] = (parent < 0 ? world[0] : world[parent]) * local[i];
```

Alternatively, give every non-root bone a valid parent when the hierarchy is built, either in Java (`RagdollBuilder.getSkinningDataSkeletonHierarchy`) or in native `RagdollBuilder::initializeSkeletonHiearachy`.

Clamping `numberOfBones` to the skeleton's own bone count would close the related overrun.

## How this was established, and what is untested

- The function and offsets come from disassembling both libraries. The Linux build keeps its symbols, and the Windows function was matched to it by structure.
- The crash dumps from the field lost their register state, so the `-1` index was not read from a dump. It follows from the faulting instruction (a load at `world + parent * 0x80`), the Java code that produces the hierarchy, and the mesh files.
- The harness was built and run on Linux, where it faults at the predicted address on every run. Its Windows branch has not been compiled.
- The PageHeap recipe has not been run against the game yet.

## Earlier reports of this crash

These threads describe a crash on aiming a firearm at a zombie. Several quote the same `PZBullet64.dll+0x269fd` frame.

| Thread | Version | Notes |
| --- | --- | --- |
| [Crash to Desktop When Aiming at a Zombie — No Mods, New Save](https://theindiestone.com/forums/topic/101607-42204-crash-to-desktop-when-aiming-at-a-zombie-no-mods-new-save/) | 42.20.4 | No mods, new save. Matches the `-1` parent case. |
| [CTD on PZBullet64.dll with mod what changes player (and zombies) model](https://theindiestone.com/forums/topic/101299-42204-ctd-on-pzbullet64dll-with-mod-what-changes-player-and-zombies-model/) | 42.20.4 | Same `+0x269fd` frame with a model-changing mod. Matches the related overrun. |
| [Crashing when aiming firearm - error included](https://steamcommunity.com/app/108600/discussions/1/595136072544947629/) | 42.21 | Steam discussion. |
| [Aiming guns at zombies causes game crash](https://theindiestone.com/forums/topic/92985-42153-fixed-it-was-my-fault-aiming-guns-at-zombies-causes-game-crash/) | 42.15.3 | Custom player model. |
| [game crashing when aiming a gun](https://theindiestone.com/forums/index.php?/topic/73381-4200-rev25072-game-crashing-when-aiming-a-gun/) | 42.0.0 | Crashes on aiming at a zombie, never with a melee weapon. |
