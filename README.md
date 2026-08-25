# nsmbtool

The NSMB-specific half of the toolchain.

[NCPatcher](https://github.com/TheGameratorT/NCPatcher) stays game-agnostic: it knows DS
containers, module graphs and file IDs, and nothing about *New Super Mario Bros.* Everything that
is knowledge of this particular game lives here instead. The two programs talk through the JSON
dumps NCPatcher writes — never through a shared library — so neither repository has to build before
the other.

```
nsmbtool reference list          Revisions in the store, and which this project uses
nsmbtool reference use <rev>     Pin a branch, tag or commit, then sync
nsmbtool reference sync          Materialise the locked revision and write .ncpatcher.env
nsmbtool reference path [<rev>]  Print a revision's directory
nsmbtool reference gc            Remove revisions no known project names
nsmbtool glue                    Generate the glue headers and the editor contracts
nsmbtool stamp --out <file>      Write the build identifier the crash screen shows
```

## Why a lock file

The reference is a real build input. It carries `include/`, `symbols9.c`, `symbols7.c` and the
`overlays9.yaml` overlay catalog, so which revision you build against changes what comes out.

The way that revision has been chosen until now is an environment variable:

```sh
export NSMBREF_ROOT=/somewhere/NSMB-Code-Reference
```

A shell has exactly one of those, and the repository has no tags — a revision is only nameable by
its hash. So a machine with two projects on two revisions cannot build both, and, worse, the losing
project still builds: against the wrong headers, the wrong symbols and the wrong overlay catalog,
silently.

`nsmbref.lock` moves the answer into the project, next to `ncpatcher.yaml`, under version control,
where a diff shows it changing:

```yaml
version: 1
reference:
  repo: https://github.com/MammaMiaTeam/NSMB-Code-Reference
  rev: ac823910c51fca86145f6cf136728fa1d5ee6bc7
```

Commit it. The revision is always a full hash: an abbreviation names one commit only until the
repository grows, which is not what a pin is for, so both the reader and the writer refuse one.

## Getting started

```sh
cd my-project
nsmbtool reference use master     # or a tag, or a commit
```

That resolves the name to a full hash, writes `nsmbref.lock`, checks the revision out into the
store, and generates `.ncpatcher.env`. From then on, a clean checkout needs only:

```sh
nsmbtool reference sync && ncpatcher build --all-variants
```

`sync` is idempotent and does not touch the network when the revision is already present, so it is
safe to leave in a build script. `--offline` turns a fetch it would have needed into an error
instead, which is what CI and packaging builds want.

## The generated environment file

`sync` writes `<project>/.ncpatcher.env`:

```
NSMBREF_ROOT=/home/you/.local/share/nsmbtool/reference/ac823910c5...
```

NCPatcher reads that file before `${env.*}` resolves, and **what is in it overrides the ambient
environment**. That inversion of the usual dotenv precedence is the whole point: a stale global
`NSMBREF_ROOT` in a shell profile must not decide what a project builds against. The command line
still wins over both — `--var` and the explicit options are the caller deliberately overriding the
project, one invocation at a time — and `ncpatcher --no-env-file` ignores the file entirely.

**Add `.ncpatcher.env` to `.gitignore`.** It is generated, machine-specific and absolute; committing
it hands every other clone a directory that does not exist. `sync` says so if you have not.

The file is regenerated in full every time and holds `NSMBREF_ROOT` and nothing else, so anything
added to it by hand is dropped on the next sync. Machine-specific variables belong in your shell
profile, `NSMB_NITRO_ROOT` above all: the converted Nitro SDK headers are private, cannot be
fetched, and their location is a property of the machine rather than of the project. That is also
why the lock does not mention them.

## The store

```
$XDG_DATA_HOME/nsmbtool/          (%LOCALAPPDATA%\nsmbtool on Windows)
  mirror.git/              one bare clone: the history, once
  reference/<full-sha>/    one worktree per revision
  projects.txt             which projects have synced, so gc knows what is still in use
```

Every revision shares the one object store, so a second pin costs a working tree rather than another
copy of the history. `NSMBTOOL_STORE` moves the whole thing — useful for a CI cache, and for
running the tests against a scratch store.

A revision is a git worktree rather than an extracted archive so that the checkout can be *verified*:
`rev-parse HEAD` answers what is actually there, so a sync interrupted halfway is detected and redone
instead of being trusted because the directory exists. The same check notices if you have edited a
reference in place — that changes build inputs without changing the lock, so `sync` warns about it.

`gc` removes revisions that no known project's lock names. It leaves the mirror alone, so a revision
it removed can be checked out again with no network at all. `--dry-run` lists what would go.

## Generating the glue assets

```sh
nsmbtool glue --graph build/generated/modules.json \
              --manifest build/generated/files.json \
              --out build/generated
```

Those are the defaults, so in a project laid out the ordinary way `nsmbtool glue` on its own does
the same thing. It reads two NCPatcher dumps — `ncpatcher.modules/1` and `ncpatcher.files/1` — and
writes:

```
<out>/include/objectids/<module>.hpp             ObjectID::<Module>::<Name>
<out>/include/object_registry.hpp                Game::ExtendedObjectsStart / Count
<out>/include/scene_overlay_registry.hpp         getSceneOverlayID
<out>/include/fid.hpp                            the ""fid literal, for every file in the ROM
<out>/include/generated/glue/extended_profiles.hpp
<out>/include/generated/glue/level_data_getters.hpp
<out>/include/generated/glue/extended_stageobjects.hpp
<out>/level_data.json                            nsmbtool.leveldata/1
<out>/stageobjects.json                          nsmbtool.stageobjects/1
```

It belongs in a `post-files` hook, not a `pre-build` one: `fid.hpp` can only be written once
insertion has settled the ROM's file IDs, and it has to exist before the code that references them
compiles. That point in the build is exactly what the phase is for.

```yaml
hooks:
  - name: Generate the glue assets
    run: nsmbtool glue --graph "${ncp.moduleDump}" --manifest "${ncp.fileDump}"
    when: post-files
```

A file is rewritten only when its bytes change. `fid.hpp` is included nearly everywhere, and a hook
that runs on every build would otherwise give it a newer timestamp than every object file and
rebuild the project from scratch, every time, for nothing.

### What modules declare

Everything below is passed through by NCPatcher untouched, under `extra` — it has no idea what any
of it means, which is the point.

```yaml
# module.yaml
id: Coop

level-data:              # module level
  canFly: flag           # present or absent
  canDie: u32            # a value
  someArray: u8[8]       # a fixed array
  someVector: u32[?]     # a count, then that many elements

components:
  - Vanilla:
      objects:           # component level
        - name: CoopFlagActor
          type: actor    # actor | scene
          header: coop/actors/CoopFlagActor.hpp
          stage: [Default, Big]
```

### Two ID spaces, which are not the same size

|  | Object ID | Stage object ID |
|---|---|---|
| Table | `ObjectProfile` / `mainExtPT` | `ObjectInfo` / `extObjInfos` |
| Base | `0x182` | `325` |
| Holds | the runtime class — the spawn vtable | placement geometry |
| Cardinality | one per object | one per `stage:` variant |
| Allocated by | this tool, at build time | the editor, per level, per unique hash |

`stage: [Default, Big]` is therefore *one* actor class placeable as *two* entries with different
geometry — `CoopFlagActor::ObjectInfo_Default` and `::ObjectInfo_Big`, both spawning the same class.
`stageobjects.json` carries no stage object ID at all, because there is no build-time answer to what
it should be.

**Identity is the hash, not either number.** A level stores the FNV-1a of `module.Object.Variant`,
and the runtime matches that against the generated table. The `0x182 +` indices are compile-time
constants that only have to agree with themselves within one build, so renumbering them invalidates
nothing; a published hash must never change, which makes `module.Object.Variant` naming a
compatibility surface.

### Three constants

`0x182` is the first object ID past the game's own table, `325` is where the vanilla stage-object
table ends, and `131` is the overlay count the game subtracts from every file ID it is handed. All
three are hardcoded here, deliberately: they are facts about how the game is *written*, not about
whatever ROM is loaded. Deriving `131` from the manifest would be actively wrong — a `create`-mode
region that adds overlay 131 changes the ROM's overlay count while every existing file ID must stay
exactly where it is.

## The build stamp

```sh
nsmbtool stamp --out build/generated/BUILDTIME
```

Writes `<short hash> <commit date>` — no trailing newline, because the game reads it as a C string
and prints it on one line of the crash screen. The result is an ordinary file, put into the ROM
through NCPatcher's `files:` like any other asset:

```yaml
files:
  BUILDTIME: build/generated/BUILDTIME

hooks:
  - name: Stamp the build
    run: nsmbtool stamp --out build/generated/BUILDTIME
    when: pre-build
```

The date is formatted from git's committer *timestamp*, so it is real UTC rather than whatever wall
clock the commit was made against. There is no default output path: where a project keeps a
generated asset is the project's convention, not this tool's.

Running outside a git repository warns and stamps `unknown` rather than failing — a source archive
has no history — but it says so, because the crash screen is the only place the string is ever read.

## The editor contract

There is no editor. What there is instead is four versioned JSON documents that together describe
everything one would need, so that the design outlives the decision not to build it yet.

| Document | Written by | Says |
|---|---|---|
| `ncpatcher.files/1` | NCPatcher, `files-dump` | the ROM's file table, and which module, component and language each entry came from |
| `ncpatcher.modules/1` | NCPatcher, `modules.dump` | the resolved module graph, with everything game-specific preserved under `extra` |
| `nsmbtool.leveldata/1` | `nsmbtool glue` | the keys a level may carry beyond the vanilla format, and their hashes |
| `nsmbtool.stageobjects/1` | `nsmbtool glue` | the placeable-object palette, one entry per `stage:` variant |

The schemas are in [`schema/`](schema) here and in NCPatcher's `schema/`. Every one carries a
`schema` field naming its version; a consumer should refuse a version it does not know rather than
guess, since the failure mode of guessing is a level written wrong.

### How they compose

`ncpatcher.files/1` is a filesystem view that reads as a diff: every entry is either vanilla,
replaced by a module, or added by one, and grouping by path across the per-variant manifests answers
which languages translate a given file. A file that exists in a module's tree but not yet in the ROM
is *pending insertion* — it has no ID, and it cannot be given one from outside.

That last point is the constraint the whole design turns on, and it belongs in any tool built on
this: **existing file IDs are never renumbered; only `z_new/` additions may move.** A game stores
file IDs in compiled code, in saves, and in already-published levels. So an editor may add files
only by putting them in a module tree and letting a build place them, which is exactly why it shows
them as pending rather than writing into the ROM itself.

`nsmbtool.stageobjects/1` is the palette. What the editor writes into a level for a placed object is
the **hash**, never either ID:

- `objectId` is which class the game spawns. It is allocated at build time, upward from `0x182`,
  **one per object** — so two variants of one class share it, and seeing the same `objectId` twice
  is correct rather than a duplicate.
- The **stage object ID** is not in the file at all. It is `325 + n`, allocated by the editor, per
  level, one `n` per distinct hash it places, contiguous from `325`. The runtime indexes its table
  by `id - 325` and relies on that being contiguous.

`nsmbtool.leveldata/1` is the schema of the block the editor writes alongside those placements. The
placed objects themselves are one such key — `glue.stageObjects` — declared by the glue module,
because the code that reads it is glue's own.

Both documents make the same promise and impose the same obligation: **the hash is the identity, and
a published hash must never change.** `module.Object.Variant` and `module.key` are therefore
compatibility surfaces. Renaming one does not fail a build; it silently orphans the data every
existing level stores under the old name.

## Building

C++20, CMake, and one dependency: yaml-cpp (found on the system, or fetched). `git` must be on
`PATH` at run time — it is how the store is managed and there is no other way in.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Linux, macOS and Windows. The Windows half of process spawning is a separate
implementation guarded by `_WIN32`, so it is invisible to a native build
elsewhere; `cmake/mingw-w64-x86_64.cmake` cross-builds it, and the test suite
runs under Wine. Paths are UTF-8 throughout and converted to UTF-16 only at the
boundary where they are handed to Windows, so a store under
`C:\Users\José\AppData\Local` works.

Tests are off by default because they drive the real git:

```sh
cmake -S . -B build -DNSMBTOOL_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build
```

They use a scratch repository on disk rather than a mock. A mocked git would only confirm that the
arguments were spelled the way the mock expected, and every failure this code actually has is in
what git does.
