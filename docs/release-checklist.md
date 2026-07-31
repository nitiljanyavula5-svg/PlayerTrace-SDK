# Release checklist

Steps to cut a PlayerTrace release (e.g. `v0.1.0`).

**Status: not started.** Nothing below may be ticked on the strength of local
results alone. Several gates can only be evaluated by native CI, and the release
process itself (tag, publish) has not been run. See "Audit remediation" below.

## Pre-release

### Build and test

- [ ] All CI jobs green on `main` (Windows, Ubuntu, macOS; Debug + Release).
- [ ] Warning-clean with `-DPLAYERTRACE_ENABLE_WERROR=ON` on GCC and Clang.
- [ ] **MSVC:** first native run completed with `/W4`; then `/WX` enabled in
      `.github/workflows/ci.yml` and CI re-run green. (Staged deliberately: the
      code has never been compiled by MSVC, so unknown warnings must be seen
      before they are made fatal.)
- [ ] AddressSanitizer + UndefinedBehaviorSanitizer job green on Linux.
- [ ] **ThreadSanitizer** job green. This is the only tool that can evaluate the
      admission/consent/shutdown concurrency work; it is unavailable on the
      MinGW toolchain used locally.
- [ ] Concurrency-sensitive suite repeated (`--repeat until-fail:50`) green in
      Debug and Release on every platform.
- [ ] `clang-format` produces no diffs (`--dry-run --Werror`).
- [ ] `clang-tidy` job green with the current `HeaderFilterRegex`.

### Packaging

- [ ] `find_package(playertrace)` consumer builds and runs against an install
      tree (`packaging/consumer-test`).
- [ ] Installed-legal-files test passes (`installed_legal_files`): `LICENSE`,
      `THIRD_PARTY_NOTICES.md`, and the vendored nlohmann MIT notice are present.
- [ ] Co-link test passes in both link orders (`packaging/colink-test`): an
      application may link its own SQLite alongside PlayerTrace.
- [ ] Subproject isolation test passes (`packaging/parent-test`): embedding does
      not change the parent's build type or install set.
- [ ] `PLAYERTRACE_USE_SYSTEM_JSON=ON` builds, installs, and is consumable by an
      isolated `find_package` project.
- [ ] vcpkg manifest validated; a real, tested `builtin-baseline` pinned **if**
      the `system-json` feature is to be supported from a manifest build. Never
      invent a commit hash.

### Content

- [ ] Version bumped consistently:
  - [ ] `project(... VERSION x.y.z)` in `CMakeLists.txt`
  - [ ] `PLAYERTRACE_VERSION_*` in `include/playertrace/version.hpp`
  - [ ] `vcpkg.json` `version`
- [ ] `CHANGELOG.md` updated: move items from `Unreleased` into the new version
      with the release date; update the compare/tag links.
- [ ] Docs reviewed for accuracy against the code (architecture, reliability,
      privacy, event-schema).
- [ ] RFC status changed from Draft to Approved — only after the above are done.
- [ ] Public API reviewed for accidental exposure of implementation types
      (`grep -RE "nlohmann|sqlite3|fstream|mutex|thread" include/` returns
      nothing).
- [ ] README badges point at the real GitHub owner/repo; the CI badge is added
      back only once the workflow has actually run.
- [ ] `HOMEPAGE_URL` in `CMakeLists.txt`, `homepage` in `vcpkg.json`, and the
      CHANGELOG links use the real repository slug (currently placeholders).

## Tag & publish

- [ ] Create an annotated tag: `git tag -a v0.1.0 -m "PlayerTrace 0.1.0"`.
- [ ] Push the tag: `git push origin v0.1.0`.
- [ ] Create the GitHub release from the tag; paste the changelog section.
- [ ] Attach or reference the source archive.

## Post-release

- [ ] Open a new `Unreleased` section in `CHANGELOG.md`.
- [ ] Bump to the next development version if appropriate.
- [ ] File/triage issues for anything deferred during the release.

---

## Audit remediation

An independent audit of the v0.1.0 release candidate produced 21 findings. Every
confirmed finding that could be fixed and verified locally was remediated; see
the `Unreleased` section of `CHANGELOG.md`. The remainder are the CI-only items
listed above.

Regenerating the SQLite symbol-prefix header is part of any dependency bump:

```bash
python tools/generate_sqlite_prefix.py
```
