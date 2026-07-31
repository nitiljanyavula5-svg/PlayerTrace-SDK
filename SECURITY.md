# Security Policy

## Supported versions

PlayerTrace is pre-1.0. Security fixes are applied to the latest `0.x` release.

| Version | Supported |
|---------|-----------|
| 0.1.x   | ✅        |

## Reporting a vulnerability

Please report suspected vulnerabilities privately. Do **not** open a public issue
for security problems.

- Use GitHub's private "Report a vulnerability" (Security Advisories) on the
  repository, or
- email the maintainers at the address listed on the project page.

Include a description, reproduction steps, affected version/commit, and impact.
We aim to acknowledge reports within a few business days and to provide a fix or
mitigation timeline after triage.

## Security-relevant design notes

- **No networking in v0.1.** The SDK does not open sockets or make HTTP requests.
  There is no remote attack surface from the SDK itself in this release.
- **No secrets.** There are no hardcoded API keys, tokens, or credentials.
- **Local data.** Events are written to a local SQLite database and, by default,
  a local NDJSON file. Protect these files with appropriate filesystem
  permissions; they may contain whatever properties your game records.
- **Untrusted storage.** The SQLite store is treated defensively: corrupted or
  malformed rows are quarantined rather than trusted, and a database written by a
  newer schema version is refused.
- **Input handling.** Event names and properties are validated; oversized,
  malformed, or reserved inputs are rejected before entering the pipeline.
- **Third-party code.** SQLite, nlohmann/json, and Catch2 are vendored under
  `third_party/` at pinned versions. Track upstream advisories for those
  components and update the vendored copies as needed.

## Privacy

Privacy considerations (what is and isn't collected, consent, anonymization) are
documented separately in [docs/privacy.md](docs/privacy.md).
