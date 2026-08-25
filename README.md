# nsmbtool

The NSMB-specific half of the toolchain.

[NCPatcher](https://github.com/TheGameratorT/NCPatcher) stays game-agnostic: it knows DS
containers, module graphs and file IDs, and nothing about *New Super Mario Bros.* Everything that
is knowledge of this particular game lives here instead. The two programs talk through the JSON
dumps NCPatcher writes — never through a shared library — so neither repository has to build before
the other.

Today this manages the **code reference**. Code generation from NCPatcher's dumps comes next.

```
nsmbtool reference list          Revisions in the store, and which this project uses
nsmbtool reference use <rev>     Pin a branch, tag or commit, then sync
nsmbtool reference sync          Materialise the locked revision and write .ncpatcher.env
nsmbtool reference path [<rev>]  Print a revision's directory
nsmbtool reference gc            Remove revisions no known project names
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
cmake -S . -B build -DNH_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build
```

They use a scratch repository on disk rather than a mock. A mocked git would only confirm that the
arguments were spelled the way the mock expected, and every failure this code actually has is in
what git does.
