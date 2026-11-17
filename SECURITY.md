# Security Policy

Terila is pre-alpha. Only the latest release receives security fixes.

## Reporting a vulnerability

Do not open a public issue for security problems. Use
[GitHub Security Advisories](https://github.com/melikeymen/terila/security/advisories/new)
so the report stays private until a fix ships.

Include:

- Terila version and Minecraft protocol version
- Server OS
- Steps to reproduce
- What you expected vs what happened
- Impact: crash, dupe, corruption, privilege escalation, RCE

Acknowledgment within 72 hours. We do not run a bug bounty.

## In scope

- Item duplication
- Remote crash from a malformed packet
- Authentication bypass
- World data corruption from client input
- Remote code execution through NBT, chunk or packet parsing

## Out of scope

- Missing features (see roadmap)
- Client-side desync with no server-side effect
- Crashes caused by a hand-edited config or a corrupt world file
- Performance issues

## Disclosure

After a fix is released we publish a GitHub Security Advisory. The reporter
is credited unless they ask otherwise.
