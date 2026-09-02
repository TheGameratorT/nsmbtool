# Putting glue support into a level editor

Everything an editor needs in order to read, show, write and preserve the data a patched ROM
expects in a level, written down so the next editor does not have to re-derive it from the runtime.

The worked example throughout is NSMB Editor 5, which implements all of it. Where a decision could
have gone either way, the reason it went the way it did is given, because those are the places a
second implementation is most likely to differ and be silently wrong.

There are four separable pieces, and they are worth doing in this order:

| | Piece | Without it |
|---|---|---|
| 1 | Read the two generated JSON documents | nothing below can name anything |
| 2 | Parse and write back the glue block | a save drops every custom key in the level |
| 3 | Register stage objects, so they become placeable | modules ship objects nobody can put in a level |
| 4 | Build the ROM for a chosen variant | multi-language projects can only be built from a shell |

Pieces 1 and 2 are worth having even alone: an editor that does nothing but *not destroy* the glue
block is already better than one that silently truncates it.

## 1. What the build tells you

`nsmbtool glue` writes two documents next to the ROM, by convention in `build/generated/`:

```
level_data.json     nsmbtool.leveldata/1      the keys a level may carry
stageobjects.json   nsmbtool.stageobjects/1   the objects a level may place
```

Their schemas live in [`../schema/`](../schema) and every field is documented there; that is the
normative description and this document does not repeat it.

Four things about consuming them:

- **Refuse a `schema` you do not know**, rather than reading what you recognise and ignoring the
  rest. Guessing at a document written by a newer build ends in a level written wrong, which is
  discovered much later than a refusal is.
- **Where they are is the project's convention, not the tool's.** Make the directory configurable
  and default it to `build/generated` relative to the ROM. NSMBe5 keeps it in its settings as
  `NCPatcherGeneratedPath`.
- **Nothing here is required.** A vanilla ROM has no such files, and that is not an error — it means
  this ROM was not built from a module project. Everything below has to degrade to "no palette, no
  declared keys" without complaint.
- **Reload them when the ROM changes**, not once at startup. NSMBe5 calls `ModuleData.Load(romfile)`
  from `LoadROMDependentData`.

The one thing an editor loads from these documents and must then keep hold of is the mapping from
**hash to name**. Everything a level stores is hashes; everything a person reads is names.

## 2. The glue block

### Where it is

NSMB's level header block (block 0) is 32 bytes. A patched game decides there is glue data by that
block being *longer* than 32, and reads the glue block starting at byte 32 of it —
`Glue::Level::hasGlueBlock` and `getGlueBlock` in the glue module.

So: the block is the tail of block 0, the first 32 bytes are untouched vanilla header, and an editor
that wants to say "this level has no glue data" writes block 0 back at exactly 32 bytes. There is no
flag to clear; the length *is* the flag.

### Layout

All values are little-endian `u32`. Offsets are counted **from the glue block's own byte 0**, not
from the start of block 0 and not from the start of the level file.

```
u32   flagCount
u32   flagHashes[flagCount]
{ u32 hash; u32 offset; }  entries[]      terminated by an entry whose hash is 0
      payload bytes, addressed by the offsets above
```

A **flag** is presence and nothing else — it carries no payload, and its hash appears only in that
first array. A **value** is an entry in the table plus bytes at its offset.

### The one hard part: payloads have no length

The block records where each payload *starts* and never how long it is. The runtime does not care:
it knows the declared type of the key it is looking for, so it reads the right number of bytes and
stops. An editor does not have that luxury, because it must round-trip keys it has never heard of.

The length of a payload is therefore **the distance to the next payload**, and the last one runs to
the end of the block. Which means parsing is:

1. Read the flags, then walk the entry table to its zero terminator. Remember where the table ended
   — that is where the payload area begins.
2. Sort the entries **by offset**. The table is not required to be in offset order.
3. Each payload runs from its own offset to the next entry's offset in that sorted order; the last
   runs to the end of the block.
4. Keep the values in **table order** for writing back, not in offset order. Both orders are legal;
   preserving the one the file had keeps a save from producing a gratuitous diff.

And it means several layouts are unreadable rather than merely odd. Refuse them, do not repair them:

| Refuse | Because |
|---|---|
| a block length that is not a multiple of 4 | every field in it is a word |
| a block shorter than 12 bytes | a flag count and a terminator do not fit |
| a `flagCount` that does not fit the block | compute in 64-bit so a nonsense count cannot overflow the check itself |
| an entry table with no terminator | the payload area has no start |
| an offset outside the payload area | it would overlap the table |
| the same hash twice | which one is the value? |
| two entries at the same offset | neither has a length |

The temptation is to be lenient and salvage what parses. Do not: an unreadable block that gets
"read" as far as it goes and then rewritten is exactly how the rest of it disappears. NSMBe5 treats
a parse failure as *the level opens, the glue block is not editable, and block 0 is written back
byte for byte as it came in* — see `NSMBLevel.LevelData` / `LevelDataError`.

### Writing it back

Serialising is the mirror image, with four rules that are not obvious:

- **An empty block writes 32 bytes**, i.e. no glue block at all. Not a block with zero flags and a
  bare terminator.
- **Pad each payload up to a multiple of 4.** The game reads payloads as the types they were
  declared as, and a `u32` two bytes into an unaligned payload is not something the ARM9 will read
  the way you meant. Padding also keeps block 0's own length a multiple of 4, which the blocks after
  it depend on.
- **Refuse a zero hash**, for a flag or a value. Zero terminates the entry table, so it cannot also
  name a key. No name hashes to it, but a level from somewhere else could still carry it.
- **Refuse a zero-length value.** It would start where its neighbour does, and then neither could be
  read back. The key should be removed instead.

Serialise **before you commit anything**. NSMBe5's OK handler builds the new block, calls
`ToHeaderBlock` as a dry run, and only then assigns to the level — so a block that cannot be written
leaves the level exactly as it was rather than half-edited.

### Payload encodings

`dataType` in `level_data.json` says how to read a payload, and `typeName` says what one element is:

| `dataType` | Payload |
|---|---|
| `flag` | none — the hash is in the flag array instead |
| `value` | one element |
| `array` | `arraySize` elements |
| `sizedArray` | a `u32` count, then that many elements |

An editor can size the fixed-width primitives — `u8`/`bool`, `u16`, `u32`, `s8`, `s16`/`fx16`,
`s32`/`fx32` — and no others. A `typeName` naming a struct is opaque, and the honest thing to do is
show the payload as raw bytes and let it be edited that way. NSMBe5 does exactly that, and also
falls back to raw bytes whenever the stored payload disagrees in size with the declaration, on the
grounds that showing something wrong as if it were right is worse than showing bytes.

### The preservation rule

**A key the build does not declare must survive a save untouched.** The level was authored against
some build, and the fact that this editor is looking at a different one is not evidence the data is
junk. Show it by its hash — `0xdeadbeef` — carry the bytes across, and let it be deleted only
deliberately.

The same applies one level down. A key stored as *both* a flag and a value is not something a
declaration produces, but it is representable, so keep both: NSMBe5 matches an incoming hash to a
row of the same shape first, then to any unclaimed row of that hash, then makes a new row. An
implementation that keys rows on the hash alone quietly loses one of the two.

## 3. Registration: making objects placeable

### Two ID spaces

They are different tables of different sizes and conflating them produces objects that spawn with
the wrong geometry rather than a clean failure.

| | Object ID | Stage object ID |
|---|---|---|
| Table | `ObjectProfile` / `mainExtPT` | `ObjectInfo` / `extObjInfos` |
| Base | `0x182` | `326` |
| Cardinality | one per object | one per `stage:` variant |
| Allocated by | the build | **the editor**, per level |
| In the palette? | yes, as `objectId` | **no — there is no build-time answer** |

`stage: [Default, Big]` is one class placeable as two entries with different geometry. Two palette
entries with the same `objectId` is correct, not a duplicate.

### What a level stores

Two separate things, in two separate places:

- **The registration** is the level-data key `glue.stageObjects`, a `sizedArray` of `u32` in which
  **entry `n` is the hash of the object that stage object ID `326 + n` spawns**. The index is the
  ID; the array holds no IDs itself; one entry covers however many placements refer to it.
- **The placement** is an ordinary vanilla stage object in the `StageObjs` block with `id = 326 + n`.
  That block is the only one the game walks, so being in it is what makes the object exist. Placing
  the same object twice is two entries with the same id, exactly as for a vanilla one.

At load time the runtime resolves each registered hash against the palette and fills its extension
of the id-indexed tables at `n`. A hash this build does not have becomes the `325` sentinel and does
not spawn — which is also why `325` is not a table bound: it is a value meaning "spawns nothing",
and the vanilla tables really do hold 326 entries.

`326` itself should come from `stageObjectIdBase` in the palette rather than being hardcoded a
second time. If the editor already has its own constant for the vanilla count, compare them and warn
on disagreement instead of picking one.

### Register in the level-data UI, not at drop time

The alternative — allocate an ID when the user drops an object — sounds friendlier and is much
worse: it hides an allocation that the user cannot see, cannot reorder and cannot reclaim. Making
registration an explicit list in the level-data window means the ID assignment is visible, and the
object picker then has nothing to do but list what is registered:

```
325: <the last vanilla entry>         <- vanilla, named by the ROM
326: coop.CoopFlagActor.Default       <- registered, entry 0
327: coop.CoopFlagActor.Big           <- registered, entry 1
```

Registration is append-only in NSMBe5, duplicates are blocked by filtering them out of the palette
dropdown, and the pane writes through to the same stored value the generic key editor reads — one
store with a nicer front end, not a second store.

### The index-is-the-ID trap

Removing a registration shifts every ID above it, which turns already-placed objects into different
objects. Two guards, and both are needed:

- **Refuse to remove** an entry while the level has placements under its ID. Say how many.
- **On save, remap.** Track where each originally-registered entry ended up — NSMBe5 keeps a
  parallel `registrationOrigin` list of provenance indices rather than doing index arithmetic — and
  rewrite every placed object's `id` through that map in one pass. Any placement left naming an ID
  that no longer exists blocks the save with a message. That second check is what catches simply
  unticking the whole key while objects are still placed.

### Removing the key is not the same as emptying it

Registering nothing must remove `glue.stageObjects` from the block entirely. A present-but-empty
array is a fault in whatever generated it, and the runtime's debug build says so.

## 4. Where stage object IDs ≥ 326 leak into the rest of an editor

This is the part that will not be in your plan, and it is where an editor written for 326 vanilla
IDs breaks. Audit everything indexed by a stage object id. In NSMBe5 there were three, all of which
misbehaved *silently or fatally* rather than erroring:

- **A `bool[326]` of valid sprites**, indexed by type in two render paths: an out-of-range crash on
  repaint the moment a module object is placed. It now includes the registered objects, marked
  valid unconditionally — a module object is compiled into module code rather than into one of the
  game's object banks, so there is no sprite set for it to be missing from.
- **The ROM's object-id table lookup**, `objectIDTable + type*2` out of overlay 0: for a type past
  the table that reads *past the end* and returns garbage as the class id. It became one method on
  the level — ROM table below the base, palette `objectId` above it, `-1` for an id naming nothing —
  and every call site now goes through it. `-1` is what makes an object placed against a build you
  are not looking at render as a broken sprite instead of as a random vanilla one.
- **Per-class settings lookups**, which are keyed on the class id and so inherit the fix above.

The shape of the fix generalises: **the level, not the ROM, is what can answer a question about a
stage object id**, because past 326 the answer depends on what that level registered.

Two things NSMBe5 does *not* do, in case they matter to you: module objects have no named nybble
fields (that would need a per-class entry in the editor's settings XML, which the build could
generate), and they draw as the default sprite box.

## 5. Building a variant

A project with variants — usually languages — cannot be built by an editor that only knows one
build command, and the failure is not loud.

Ask the project rather than parsing its YAML:

```sh
ncpatcher -C <project> config dump --json
```

The document goes to clean stdout with diagnostics on stderr, and its `variants` object lists them
in declaration order. Offer that list plus a "(none)" entry, remember the choice, and pass it as a
**global** option ahead of the subcommand:

```sh
ncpatcher -C <project> --variant fr build
```

Do not offer `--all-variants` from an editor: it patches one opened ROM, and nine outputs have
nowhere to go.

Two small things worth copying: only spawn `ncpatcher` when the dropdown is actually opened, since
doing it on every ROM load is a visible stall; and keep a stored variant that the project no longer
lists, rather than silently resetting it to none. An empty variant list means the project configures
none — which is a different thing from the query having failed, and should read differently.

### Do not re-derive which module supplies what

The temptation, once an editor has the module graph, is to work out for itself which module's copy
of a file wins: layering, module order, `into:` prefixes, the `_narc` archive-folder convention,
component glob subtraction, brace expansion. Resist it. Two implementations of precedence rules
cannot be kept in step by testing either one alone, and the editor's copy is the one that silently
disagrees with the ROM the user ships.

Ask instead:

```sh
ncpatcher -C <project> files plan --variant fr --json
```

The answer is the same `ncpatcher.files/1` document a build writes — every id, path, size, action
and provenance — with `planned: true` and prospective ids for the files a build would append under
`z_new/`. Nothing is built and no toolchain is needed. An editor's file list is then a join of three
things it does not derive: that plan, the last build's `files-dump` manifest, and the bytes each
named source currently holds. "Pending" is exactly "those disagree".

Two fields on the entries are worth knowing about:

- `source-missing` marks a destination whose source a `pre-build` hook has not generated yet. Its
  id, path and provenance are still real; only its size is unknown. Report the last build for it
  rather than a pending mark the user can never clear.
- `members` appears on an archive the run edited, and carries the index, path, module, component
  and variant of each member it wrote. A container entry can hold no more provenance than its
  members agree on, which for members from three modules is none — so this array is where the
  answer is if you want to show *which* member of a `.narc` is stale. Members a build would not
  write are not listed; read those from the container itself.

`config dump --json` grew three fields for the same reason: `rom-banner` at the root, `banner` per
variant, and `module-variants` per variant. The last is what selects a module's layer when its tree
names layers differently from the project's variants — an editor that guesses it writes an edit into
a layer no build reads.

## 6. Verifying it

The cheap checks that would have caught every bug found while writing the reference implementation:

- **Round-trip.** Parse and re-serialise a block with several keys, one of them undeclared, and
  compare bytes. Then do it with the entry table deliberately out of offset order.
- **Every refusal above** gets a test that asserts it refuses. They are one line each and they are
  the whole difference between "corrupts rare levels" and "says what is wrong".
- **A level that registers two objects** should produce exactly this, for
  `coop.CoopFlagActor.Default` and `.Big` — hand-checkable against the runtime reader:

  ```
  00000000   flagCount = 0
  6DB68F3C   glue.stageObjects
  00000014   -> offset 20
  00000000   terminator
  00000002   count = 2
  F46A44B9   coop.CoopFlagActor.Default   -> id 326
  27E61112   coop.CoopFlagActor.Big       -> id 327
  ```

  Offset 20 is `4 + 0 flags + 8 entry + 8 terminator`, so `findData` lands on the count.

- **The hash function**, against known values. It is FNV-1a/32 over the bytes of the key with no
  terminator: `hash = 2166136261`, then per byte `hash ^= byte; hash *= 16777619`. Keys are
  `lowercase-module.key` and `lowercase-module.Object.Variant`. Check `coop.canFly` → `0xc8dfeb91`
  and `coop.CoopFlagActor.Default` → `0xf46a44b9`.
- **Actually boot a level with a placed object.** A block that assembles and a palette that lists
  things prove nothing about whether the game spawns it.

## 7. What must never change

The hash is the identity — not the object id, not the stage object id, not the position in any
table. Which makes three names compatibility surfaces, in the module rather than in the editor:
`module.key`, `module.Object.Variant`, and the name `glue.stageObjects` itself, which is the one
string an editor has to hardcode because something must say which key holds the registration.

Renaming any of them fails no build. It orphans the data every existing level stores under the old
name, and the editor will show it as a bare hash — which is at least visible, and is the reason the
preservation rule in §2 exists.
