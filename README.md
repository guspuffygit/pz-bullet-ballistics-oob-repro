# Out-of-bounds read in `PZBallisticsTarget::updateSkeletonBoneTransforms` (PZBullet64.dll)

## Summary

- `PZBallisticsTarget::updateSkeletonBoneTransforms` walks `numberOfBones` bones over a native bone table that holds 35 entries. Nothing compares the two.
- `numberOfBones` is the bone count of the target character's body model. The vanilla skeleton bodies, `Male_Skeleton.x` and `Female_Skeleton.x`, have 45 bones.
- The walk then reads parent indices from the memory after the bone table and uses them to index the world-transform array. A junk index faults and the client closes to desktop at `PZBullet64.dll+0x269fd`.
- Aiming a firearm at a skeleton zombie therefore overruns the table. No mod is needed. A modded body mesh with extra bones does the same.
- Three crash logs from two Windows clients on 42.22.2 show `numberOfBones` = 45. One was produced on demand by aiming at a skeleton zombie.

| | |
| --- | --- |
| Game | Project Zomboid 42.22.2, `pzbullet=1.0.0.28`, revision `3d39148846` |
| `PZBullet64.dll` SHA-256 | `76141df900f08ef71c0808bcd6248c929c47b8c5900b86af4875131a780897ef` |
| `libPZBullet64.so` SHA-256 | `b5d6178a789280c3bc4787ddf24f1bd44e4037d2d097bf6b4e5b4174c9b67354` |

## Where

| | |
| --- | --- |
| Function | `PZBallisticsTarget::updateSkeletonBoneTransforms(int numberOfBones, float* data)` |
| Windows | `PZBullet64.dll` RVA `0x26590`, faulting load at RVA `0x269fd` |
| Linux | `libPZBullet64.so` `0x172ef0` (unstripped, the source of the symbol name) |
| JNI entry | `Java_zombie_core_physics_Bullet_updateBallisticsTargetSkeleton` |
| Java callers | `BallisticsTarget.add()` from `CombatManager.processBallisticsTargets` (aim), and `BallisticsTarget.update()` from `IsoGameCharacter.updateInternal` (each tick while the target is live) |

## Root cause

The walk builds each bone's world transform from its parent's.

```cpp
world[0] = this->transform;
for (int i = 1; i < numberOfBones; i++)
    world[i] = world[ pose->skeleton->bones[i].parent ] * local[i];
```

- Bone records are 0x20 bytes with the parent index at offset 0.
- `world` and `local` hold 0x80-byte `btTransform` records and are sized from `numberOfBones` on each call.
- The bone table is built once by `Bullet.initializeRagdollSkeleton(SkeletonBone.count(), boneHierarchy)` and holds 35 records.
- The loop bound is the caller's `numberOfBones`. The parent index is used unchecked.

### How Java produces a count above 35

1. `BallisticsTarget.initialize()` sets `numberOfBones = character.getAnimationPlayer().boneTransforms.length`.
2. `AnimationPlayer.setModel` sizes `boneTransforms` from the body model's `SkinningData.numBones()`.
3. `ModelManager.getBodyModel` returns `skinned/malebody` or `skinned/femalebody`, or `skinned/Male_Skeleton` or `skinned/Female_Skeleton` when `IsoZombie.isSkeleton()` is true. Each is loaded against the shared animations model.
4. `ImportedSkeleton.processAiScene` starts from the 35 animation bones and appends every node under the root bone that it does not already know.

The vanilla skeleton meshes carry ten end bones that the normal bodies lack.

```
Bip01_HeadNub  Bip01_BackPackNub  Bip01_DressBackNub  Bip01_DressFrontNub
Bip01_L_Finger0Nub  Bip01_L_Finger1Nub  Bip01_R_Finger0Nub  Bip01_R_Finger1Nub
Bip01_L_Toe0Nub  Bip01_R_Toe0Nub
```

That gives 35 + 10 = 45. A live 42.22.2 client reports 35 bones for the male and female body models and 45 for both skeleton models.

## Evidence from the crash logs

Two `hs_err` logs from the field fault at `PZBullet64.dll+0x269fd` on `MainThread` with the same register pattern.

| Register | Value | Meaning |
| --- | --- | --- |
| `R14`, `R15` | `0x2d` | `numberOfBones` = 45 |
| `RBX` | `0x1680` | 45 × 0x80, the byte size of `world[]` |
| `R9` | `0x25` | loop index 37 |
| `R10` | `0x4a0` | 37 × 0x20, the offset of bone record 37 |
| `RCX` | `0x43600` in one log, `0x30ba308000` in the other | parent index × 0x80 |
| `RAX` | heap pointer | base of `world[]` |

- The faulting address equals `RAX + RCX + 0x50` in both logs.
- The parent index differs between the two logs (2156 and `0x61746100`), so it was read from unrelated memory. The second value is ASCII text.
- Records 35 and 36 held values that happened to stay mapped, so the fault came at record 37.
- A third log, produced on demand (see Reproduce), has the same `R14`, `R15` and `RBX`, with the fault at loop index 35 and a parent index of `0x0c000000`.
- One log's Java stack enters through `CombatManager.updateReticle` → `processBallisticsTargets` → `BallisticsTarget.add`. The other enters through `IsoZombie.update` → `BallisticsTarget.update`.

## Reproduce

No mods are needed. The skeleton body models ship with the base game.

1. Launch 42.22.2 on Windows with the `-debug` launch option and start a solo game.
2. Press F11 to open the debug options and tick `Model.Force.Skeleton`. `IsoZombie.isSkeleton()` then returns true for every zombie, so each one switches to the 45-bone skeleton body model.
3. Equip a loaded firearm and aim at a zombie in range.
4. The client closes to desktop at `PZBullet64.dll+0x269fd`.

Whether a given call faults depends on what follows the bone table in the heap, so a run may need more than one aim.

## Suggested fix

Bound the walk by the skeleton's own bone count and reject a parent index outside the buffer. The field name `boneCount` below is a placeholder.

```cpp
int count = std::min(numberOfBones, pose->skeleton->boneCount);
for (int i = 1; i < count; i++) {
    int parent = bones[i].parent;
    if (parent < 0 || parent >= count) parent = 0;
    world[i] = world[parent] * local[i];
}
```

The same clamp can be applied in Java, where `BallisticsTarget.initialize()` could use `Math.min(boneTransforms.length, SkeletonBone.count())`.

## How this was established, and what is untested

- The function and offsets come from disassembling both libraries. The Linux build keeps its symbols, and the Windows function was matched to it by structure.
- The bone count, loop index and junk parent index come from the register dumps in two `hs_err` logs.
- The path from body mesh to `numberOfBones` comes from reading the decompiled Java.
- On a live 42.22.2 client, non-skeleton characters had 35 bones and no bone other than the root had parent `-1`, so the walk stays in bounds for them. Aiming at them under full PageHeap did not fault.
- The same client reported 45 bones for both skeleton models, which load from the base game's files.
- The in-game crash was produced once by calling `setSkeleton(true)` on one zombie on a modded client and aiming a firearm at it. The `Model.Force.Skeleton` steps above make `isSkeleton()` return true the same way, and were not run on an unmodded install.
- The zombies in the field crashes were not confirmed to be skeletons. Their bone count of 45 matches the skeleton models, and the server's one body-replacing mod loads 35-bone meshes.
- An earlier version of this report blamed a `-1` parent on `Translation_Data` in the vanilla mesh. The live test disproved that.

## Earlier reports of this crash

These threads describe a crash on aiming a firearm at a zombie. Several quote the same `PZBullet64.dll+0x269fd` frame.

| Thread | Version | Notes |
| --- | --- | --- |
| [Crash to Desktop When Aiming at a Zombie — No Mods, New Save](https://theindiestone.com/forums/topic/101607-42204-crash-to-desktop-when-aiming-at-a-zombie-no-mods-new-save/) | 42.20.4 | Reported as no mods on a new save. A moderator attributed it to leftover mod files. |
| [CTD on PZBullet64.dll with mod what changes player (and zombies) model](https://theindiestone.com/forums/topic/101299-42204-ctd-on-pzbullet64dll-with-mod-what-changes-player-and-zombies-model/) | 42.20.4 | Same `+0x269fd` frame with a model-changing mod. Matches this report. |
| [Crashing when aiming firearm - error included](https://steamcommunity.com/app/108600/discussions/1/595136072544947629/) | 42.21 | Steam discussion. |
| [Aiming guns at zombies causes game crash](https://theindiestone.com/forums/topic/92985-42153-fixed-it-was-my-fault-aiming-guns-at-zombies-causes-game-crash/) | 42.15.3 | Custom player model. |
| [game crashing when aiming a gun](https://theindiestone.com/forums/index.php?/topic/73381-4200-rev25072-game-crashing-when-aiming-a-gun/) | 42.0.0 | Crashes on aiming at a zombie, never with a melee weapon. |
