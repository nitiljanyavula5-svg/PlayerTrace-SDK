# Release checklist

Steps to cut a PlayerTrace release (e.g. `v0.1.0`).

**Status: pre-release gates complete; tag and publish outstanding.**

Every ticked item below is backed by a specific green hosted CI run on `main`,
not by local results. The workflow run for commit `eacc393` was green on all 11
jobs with MSVC warnings-as-errors enabled. Items that remain unticked are those
that genuinely have not happened yet: the tag, the GitHub release, and the
post-release steps. See "Audit remediation" below.

The release-content commit that follows `eacc393` changes documentation only,
but it must still be green before the tag is created — confirm the run for the
final commit rather than relying on `eacc393`.

## Pre-release

### Build and test

- [x] All CI jobs green on `main` (Windows, Ubuntu, macOS; Debug + Release).
      All 11 jobs green for `eacc393`.
- [x] Warning-clean with `-DPLAYERTRACE_ENABLE_WERROR=ON` on GCC and Clang.
      Ubuntu (GCC) and macOS (AppleClang), Debug and Release.
- [x] **MSVC:** first native run completed with `/W4`; then `/WX` enabled in
      `.github/workflows/ci.yml` and CI re-run green. (Staged deliberately: at
      that point the code had never been compiled by MSVC, so unknown warnings
      had to be seen before they were made fatal.)
      The `/W4` run reported two C4996 `std::fopen` deprecations in FileSink.
      They were fixed at the call site with `_fsopen`/`_SH_DENYNO` rather than
      suppressed, and `/WX` was enabled only after the following run was clean.
- [x] AddressSanitizer + UndefinedBehaviorSanitizer job green on Linux.
- [x] **ThreadSanitizer** job green. This is the only tool that can evaluate the
      admission/consent/shutdown concurrency work; it is unavailable on the
      MinGW toolchain used locally.
- [x] Concurrency-sensitive suite repeated (`--repeat until-fail:50`) green in
      Debug and Release on every platform. The ThreadSanitizer job repeats the
      same suite 5 times, which is deliberate: under TSan each repetition is far
      slower, and its value is the race detector rather than the iteration
      count.
- [x] `clang-format` produces no diffs (`--dry-run --Werror`).
- [x] `clang-tidy` job green with the current `HeaderFilterRegex`.

### Packaging

- [x] `find_package(playertrace)` consumer builds and runs against an install
      tree (`packaging/consumer-test`).
- [x] Installed-legal-files test passes (`installed_legal_files`): `LICENSE`,
      `THIRD_PARTY_NOTICES.md`, and the vendored nlohmann MIT notice are present.
- [x] Co-link test passes in both link orders (`packaging/colink-test`): an
      application may link its own SQLite alongside PlayerTrace.
- [x] Subproject isolation test passes (`packaging/parent-test`): embedding does
      not change the parent's build type or install set.
- [x] `PLAYERTRACE_USE_SYSTEM_JSON=ON` builds, installs, and is consumable by an
      isolated `find_package` project.
- [x] vcpkg manifest validated. NON-BLOCKING: no `builtin-baseline` is pinned,
      deliberately — one must name a real, tested vcpkg commit and none has been
      validated for this release. The default vendored build does not need it;
      it is required only to consume the `system-json` feature from a manifest
      build. Recorded in `vcpkg.json` under `$comment-baseline`.

### Content

- [x] Version bumped consistently:
  - [x] `project(... VERSION x.y.z)` in `CMakeLists.txt`
  - [x] `PLAYERTRACE_VERSION_*` in `include/playertrace/version.hpp`
  - [x] `vcpkg.json` `version`
- [x] `CHANGELOG.md` updated: move items from `Unreleased` into the new version
      with the release date; update the compare/tag links. The audit-remediation
      sections were folded into `[0.1.0] - 2026-07-31` (0.1.0 was never
      published, so they are not a separate version), a fresh empty
      `[Unreleased]` was opened above it, and the compare/tag links now use the
      real slug.
- [x] Docs reviewed for accuracy against the code (architecture, reliability,
      privacy, event-schema). `docs/reliability.md` was corrected during the
      re-audit (the retry path no longer requeues to the front of the queue).
- [x] RFC status changed from Draft to Approved — only after the above are done.
      Approved by the project owner on 2026-07-31. The banner marking the RFC as
      a historical, non-authoritative design document is retained deliberately.
- [x] Public API reviewed for accidental exposure of implementation types
      (`grep -RE "nlohmann|sqlite3|fstream|mutex|thread" include/` returns
      nothing).
- [x] README badges point at the real GitHub owner/repo; the CI badge is added
      back only once the workflow has actually run. The live `ci.yml` badge
      replaces the static "verified on GCC/MinGW" badge, which CI disproved.
- [x] `HOMEPAGE_URL` in `CMakeLists.txt`, `homepage` in `vcpkg.json`, and the
      CHANGELOG links use the real repository slug. `contact_links` were also
      restored in `.github/ISSUE_TEMPLATE/config.yml`; both of those links 404
      until Discussions and private vulnerability reporting are enabled in
      repository settings.

## Tag & publish

- [ ] Create an annotated tag: `git tag -a v0.1.0 -m "PlayerTrace 0.1.0"`.
- [ ] Push the tag: `git push origin v0.1.0`.
- [ ] Create the GitHub release from the tag; paste the changelog section.
- [ ] Attach or reference the source archive.

## Post-release

- [ ] Open a new `Unreleased` section in `CHANGELOG.md`. An empty one was
      already added above `[0.1.0]` when the release content was prepared — fill
      it in rather than adding a second heading.
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
