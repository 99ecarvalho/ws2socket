# Contributing to ws2socket

Thanks for your interest in ws2socket! Bug reports, documentation fixes, and
code are all welcome. This guide explains how to get a change merged with as
little back-and-forth as possible.

- [Ways to contribute](#ways-to-contribute)
- [Reporting bugs](#reporting-bugs)
- [Development setup](#development-setup)
- [Making a change](#making-a-change)
- [Coding style](#coding-style)
- [Commit messages](#commit-messages)
- [Pull requests](#pull-requests)
- [Licensing of contributions](#licensing-of-contributions)

## Ways to contribute

- **Report a bug** or unexpected behavior. See [Reporting bugs](#reporting-bugs).
- **Improve the documentation.** If something was unclear or wrong, a fix is
  welcome.
- **Fix a known limitation.** The
  [Known limitations](docs/IMPLEMENTATION.md#known-limitations) and
  [Roadmap](docs/IMPLEMENTATION.md#roadmap) sections list the most useful
  work. Fuzzing and unit tests for the parsers are the top priorities.
- **Test on your platform.** Reports from other distributions, embedded
  boards, and Yocto builds help a lot.

Security vulnerabilities should **not** be reported as public issues. Follow
[SECURITY.md](SECURITY.md) instead.

## Reporting bugs

Search the [existing issues](https://github.com/99ecarvalho/ws2socket/issues)
first. If your bug is new, open an issue and include:

- the ws2socket version (`ws2socket --version`) or commit hash;
- your OS, architecture, and OpenSSL version (`openssl version`);
- the exact command line and configuration file you used;
- what you expected to happen and what happened instead;
- a debug log (`--verbose`), with anything sensitive removed;
- the client you used (browser and noVNC version, or another WebSocket
  client).

A minimal way to reproduce the problem is the most valuable thing you can
provide.

## Development setup

You need a C11 compiler, CMake 3.10 or newer, and the OpenSSL and zlib
development headers:

```bash
sudo apt-get install build-essential cmake libssl-dev zlib1g-dev doxygen
git clone https://github.com/99ecarvalho/ws2socket.git
cd ws2socket
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

To check a build with the address and undefined-behavior sanitizers:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan
```

For an overview of the code, read [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md).

### Testing your change

The integration tests in [tests/](tests/) start the built binary and drive it
with real WebSocket clients. They need Python 3 with the `websockets` package,
and the `openssl` command for the TLS tests:

```bash
sudo apt-get install python3-websockets openssl
ctest --test-dir build --output-on-failure
python3 tests/test_integration.py build/ws2socket -k token   # a subset
```

Before opening a pull request:

1. Build with no new compiler warnings.
2. Run the whole test suite. Also run it against the sanitizer build if you
   touched parsing or memory handling:
   `ASAN_OPTIONS=detect_leaks=0 python3 tests/test_integration.py build-asan/ws2socket`.
3. **Add a test** for a bug fix (one that fails without the fix) or for a new
   feature. Each test is a plain `test_*` function in `test_integration.py`.
4. If your change touches HTTP or noVNC behavior, also connect with noVNC in a
   browser as described in [docs/NOVNC_GUIDE.md](docs/NOVNC_GUIDE.md).
5. If you changed the Docker files, run `docker/build.sh`.

Describe what you tested in the pull request.

## Making a change

1. For anything larger than a small fix, **open an issue first** to discuss
   the approach. This avoids wasted work on changes that don't fit the
   project.
2. Fork the repository and create a branch from `main`:
   `git checkout -b fix/handshake-extension-negotiation`.
3. Keep each pull request focused on one topic. Unrelated clean-ups belong in
   a separate pull request.
4. Update the documentation in the same pull request when you change
   behavior: the README, the man page (`docs/ws2socket.1`), `--help`, and the
   relevant file in `docs/`.

## Coding style

Follow the style of the surrounding code:

- C11, 4-space indentation, and no tabs.
- K&R braces for control flow. A function's opening brace goes on its own
  line.
- `snake_case` for functions and variables, `_t` suffix for typedefs, and
  `UPPER_CASE` for macros.
- Return `WS_*` status codes from `common.h`, and log errors where they are
  detected.
- Use bounded string functions (`strlcpy()`, `snprintf()`), never `strcpy()`
  or `sprintf()`.
- Treat everything received from the network as untrusted. Check lengths
  before copying, and never trust sizes taken from a frame or header.
- Build with no new warnings under the project flags
  (`-Wall -Wextra -Wpedantic -Wstrict-prototypes`).
- Give every public function a Doxygen comment with `@brief`, `@param`, and
  `@return`.

New source files start with this header:

```c
/**
 * @file example.c
 * @brief One-line description
 * @author Your Name <you@example.com>
 *
 * @copyright Copyright (c) 2026 Your Name <you@example.com>
 *
 * This file is part of ws2socket. It is free software, licensed under the
 * GNU Lesser General Public License v3.0 or later. See COPYING.LESSER
 * and COPYING for details.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
```

## Commit messages

The project uses [Conventional Commits](https://www.conventionalcommits.org/):

```text
<type>(<optional scope>): <short summary in the imperative>

<optional body explaining what changed and why>
```

Common types are `feat`, `fix`, `docs`, `refactor`, `perf`, `test`, `build`,
and `chore`. For example:

```text
fix(http): return 404 for directories without a trailing slash

open() succeeds on a directory, so the server sent a 200 header with
the directory size and no body.
```

Keep the summary line under about 72 characters. Make each commit build on
its own.

## Pull requests

Before you open a pull request, check that:

- [ ] the project builds with no new warnings;
- [ ] the test suite passes, and the change has a test where practical;
- [ ] documentation and `--help` reflect any change in behavior;
- [ ] new files carry the copyright and SPDX header;
- [ ] commits follow the commit message convention.

A maintainer will review the pull request. You may be asked for changes, so
please don't take that as a rejection. It is how the code stays maintainable.

## Licensing of contributions

ws2socket is licensed under the GNU Lesser General Public License v3.0 or later
(see [COPYING.LESSER](COPYING.LESSER) and [COPYING](COPYING)). By submitting a
contribution, you agree that it is licensed under the same terms, and you
confirm that you have the right to submit it.
