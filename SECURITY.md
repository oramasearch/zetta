# Security policy

## Reporting a vulnerability

**Please do not report security vulnerabilities in public issues, discussions or pull
requests.**

Report them privately, in either of these ways:

- **Email:** [security@oramasearch.com](mailto:security@oramasearch.com)
- **GitHub:** the repository's **Security** tab → **Report a vulnerability** (GitHub
  private vulnerability reporting).

Please include:

- the affected version (a release tag or commit) and how the node was run (headed or
  headless, single node or cluster, which listeners were exposed);
- a description of the issue and its impact;
- steps to reproduce, a proof of concept, or the request that triggers it;
- any suggested fix or mitigation.

## What to expect

- We acknowledge a report within **3 business days**.
- We send an initial assessment (confirmed or not, severity, next steps) within
  **10 business days**.
- We keep you informed while we work on a fix and agree a disclosure date with you. Our
  default is to publish a fix and an advisory within **90 days** of the report, sooner for
  issues that are being actively exploited.
- With your permission, we credit you in the advisory and the release notes.

## Supported versions

Security fixes are made on the latest release line and on `dev`. Older releases do not
receive backported fixes; upgrade to the latest release.

| Version | Supported |
| --- | --- |
| Latest release (`v0.5.x`) | Yes |
| Older releases | No |

## Scope

In scope: the `zetta` binary and the code in this repository. That includes the peer
transport (mutual TLS, the join token), the HTTP / MCP control surface and its
authentication, access control and audit, the SQL and input parsers, the Studio web app as
served by the node, and the container image built from the [`Dockerfile`](Dockerfile).

Vulnerabilities in vendored third-party code under `modules/` or fetched at build time
should also be reported upstream; tell us as well when zetta is affected.

How the node is secured, and what a deployment must configure, is described in
[`docs/cluster/security.md`](docs/cluster/security.md) and
[`docs/governance/`](docs/governance/README.md).
