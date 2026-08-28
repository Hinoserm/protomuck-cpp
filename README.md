# protomuck-cpp

ProtoMUCK 2.1a0, the C++ line of the ProtoMUCK server.

ProtoMUCK is a MUCK server: a multi-user text world built around a persistent
object database, an in-game programming language called MUF (Multi-User FORTH),
and MPI, a message parsing interpreter used for lighter-weight scripting. It
descends from TinyMUD by way of TinyMUCK FB and NeonMUCK, and carries pieces of
each.

This repository holds the C++ branch, versioned `2.1a0.x`, which diverged from
the C mainline (`2.0b9.x`) around late 2013. Every source file was renamed from
`.c` to `.cpp` and the `proto2` subdirectory was promoted to the repository
root. Development after that point happened in a private Subversion repository
that was never published and no longer exists. What is here was reconstructed
from the surviving snapshots.

## Repository history

History runs from 2000-06-18 to the present: 973 commits on `main`, 1049 across
all branches. Everything before 2014-07-03 is the shared history of the original
project and preserves the authorship of its contributors.

The C++ line itself survives only as periodic snapshots, so each of the commits
below covers a span of unrecorded work rather than a single change:

| Date | Version | What changed |
|---|---|---|
| 2014-07-03 | 2.1a0.2.5 | First surviving snapshot of the C++ line |
| 2018-08-26 | 2.1a0.6.14 | Source only. Adds MCP (MUD Client Protocol) support |
| 2021-06-27 | 2.1a0.6.19 | Removes MCP support |
| 2022-01-19 | 2.1a0.6.24 | WebSocket and HTTP server work in `newhttp.cpp` |
| 2026-03-09 | 2.1a0.6.24 | Case-insensitive WebSocket `Upgrade` header match |

There is no record of the work between these dates. The gaps are real and cannot
be recovered.

MCP support exists only in the 2018 commit. To recover those four sources and
their headers:

```
git checkout 1e22b6e -- src/mcp.cpp src/mcpgui.cpp src/mcppkgs.cpp src/p_mcp.cpp
```

## Branches

| Branch | Contents |
|---|---|
| `main` | The C++ line, ending at the current state |
| `upstream-master` | The C mainline as of 2017-02-10, version 2.0b9.14.0 |
| `alynna`, `davin`, `hinoserm` | Imported Subversion development branches |
| `wiki` | Imported documentation branch |
| `fix_mpi_name_len`, `fix_resolver_empty_ptr` | Imported topic branches |

The C mainline continues to live at
[protomuck/protomuck](https://github.com/protomuck/protomuck). This repository
is a separate line of development and is not intended to merge back into it.

## Layout

```
src/      Server sources and headers (src/inc)
game/     Runtime directory: database, help text, MUF, configuration
dat/      Pristine copy of the game data, used to refresh game/
ext/      Scripts for fetching build dependencies
prj/      Visual Studio project files
xtra/     Alternate build files, SSL certificate helper, misc config
```

## Features

Enabled by default in `src/inc/config.h`:

- `NEWHTTPD`, a built-in HTTP server with WebSocket support (`src/newhttp.cpp`)
- `MUF_SOCKETS`, outbound socket primitives for MUF
- `MUF_EDIT_PRIMS`, in-game program editing primitives
- `THREADED_SQL_SUPPORT`, MySQL access from MUF on a worker thread
- `CONTROLS_SUPPORT`
- `COMPRESS`, dictionary-based property compression

## Building

Dependencies are PCRE and zlib, with OpenSSL and MySQL optional.

`configure` accepts `--with-pcre`, `--with-zlib`, `--with-ssl`, `--with-mysql`,
`--with-modules`, `--enable-ipv6`, `--enable-reslvd`, and `--enable-asroot`.
See `src/configure.in` for the full set.

`BUILDING` documents the Visual Studio 2012 path: run `ext/get_libraries.bat`,
open `proto2.sln`, and build the solution.

Be aware of two caveats before you try. `BUILDING` states that the Linux build
was left unfinished during the 2013 restructuring and that the Makefile still
needed a rewrite. Separately, this code predates C23 and modern compiler
defaults; implicit function declarations and similar constructs that were
warnings when it was written are now hard errors. Neither build path has been
verified against a current toolchain.

## License

ProtoMUCK uses a custom license, not an OSI-approved open source license. See
[LICENSE](LICENSE), which reproduces the terms from `COPYRIGHT` and
`src/inc/oldcopyright.h`.

Redistribution is permitted with attribution, but note restriction IV: the
software may not be sold, and access to it may not be charged for, without
written permission from the authors and from the authors of its components.

## Credits

ProtoMUCK is copyright 2000 by Chris Brine and Richard Taylor, and builds on
TinyMUD, TinyMUCK FB, NeonMUCK, the Pueblo protocol, MCP, and a compression
scheme from Dragon's Eye Productions. Full attributions are in
[LICENSE](LICENSE).

Contributors to the history in this repository include Chris Brine, Richard
Taylor, Alynna Trypnotk, CyberLeo, davin, Scimicat, and others recorded in the
commit log.
