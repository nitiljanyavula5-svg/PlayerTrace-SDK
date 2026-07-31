<!--
Thanks for contributing. Please read CONTRIBUTING.md if you have not already.
Keep this checklist honest — an unticked box is far more useful than a wrong tick.
-->

## What this changes

<!-- One or two sentences. Link the issue it closes, if any. -->

## Why

<!-- The problem being solved. For a bug fix, say what the incorrect behaviour
     was and what made it possible. -->

## How it was verified

<!-- Name the gates you actually ran, with results. "Tests pass" on its own is
     not evidence; a count is. -->

- [ ] `ctest` green in **Debug**
- [ ] `ctest` green in **Release**
- [ ] Concurrency suite repeated: `ctest -R playertrace_tests_threading --repeat until-fail:50`
- [ ] `clang-format --dry-run --Werror` clean
- [ ] New or changed behaviour is covered by a test that **fails without the fix**

Test count before / after:

## Contract review

PlayerTrace makes promises that are easy to break by accident. Confirm the ones
your change touches, or state why they do not apply.

- [ ] No exception can escape the public API; failures are reported as a `Status`
- [ ] `track()` does no blocking work on the caller's thread
- [ ] Nothing returns `Ok` for work that will not be processed
- [ ] Consent revocation remains fail-closed, synchronous, and durable
- [ ] The worker thread is never detached and is always joined
- [ ] Accepted events keep gap-free `(session_id, seq)` ordering
- [ ] Public headers gained no new third-party include

## Notes for reviewers

<!-- Anything you are unsure about, deliberate trade-offs, or areas that would
     benefit from a closer look. -->
