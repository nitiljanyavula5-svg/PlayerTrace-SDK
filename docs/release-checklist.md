# Release checklist

Steps to cut a PlayerTrace release (e.g. `v0.1.0`).

**Status: v0.1.0 released 2026-07-31.**

Every ticked item below is backed by a specific green hosted CI run on `main`,
not by local results. The workflow run for commit `459d300` — the commit the
`v0.1.0` tag points at — was green on all 11 jobs with MSVC warnings-as-errors
enabled. See "Audit remediation" below.

When cutting a future release, confirm the run for that release's own final
commit rather than relying on a commit named here.

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
      build. Recorded in `vcpkg.json` under `$comment-baseline`, and tracked
      after release as issue #2.

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

- [x] Create an annotated tag: `git tag -a v0.1.0 -m "PlayerTrace SDK v0.1.0"`.
      Tag object `ddd28e7`, dereferencing to commit `459d300`.
- [x] Push the tag: `git push origin v0.1.0`.
- [x] Create the GitHub release from the tag; paste the changelog section. The
      existing tag was reused (`gh release create --verify-tag`), not recreated.
      Published 2026-07-31, marked latest, not a pre-release.
- [x] Attach or reference the source archive.
      `PlayerTrace-SDK-v0.1.0.zip`, 3,179,482 bytes, 123 entries, built with
      `git archive --format=zip --prefix="PlayerTrace SDK/" v0.1.0`.
      SHA-256 `2B1C5F707C280303807FCA46AE69EE9E5BF69F7B58C4AB85035BA57196DDA377`,
      re-verified after downloading the published asset back from GitHub.

## Post-release

- [x] Open a new `Unreleased` section in `CHANGELOG.md`. An empty one sits above
      `[0.1.0]` — fill it in rather than adding a second heading.
- [x] Bump to the next development version if appropriate. **Not bumped, and
      deliberately so.** This project documents no development-version
      convention: the only stated policy is "adheres to Semantic Versioning"
      (`CHANGELOG.md`), and `include/playertrace/version.hpp` carries plain
      integer macros with no pre-release or `-dev` field to hold one. Choosing a
      number now would invent a policy rather than apply one, would make
      `PLAYERTRACE_VERSION_STRING` report a version that was never released, and
      would put a version in `vcpkg.json` that no manifest consumer can resolve.
      Bump the three version sources together as the *first* step of the next
      release, once its number is actually decided.
- [x] File/triage issues for anything deferred during the release. One item was
      genuinely deferred — the unpinned vcpkg `builtin-baseline` — and is now
      tracked as issue #2. The v0.2/v0.3 roadmap entries and the documented
      "Known limitations" are planned scope, not release deferrals, and were
      deliberately not filed as issues.

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
