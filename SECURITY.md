# Security Policy

## Supported code line

Security fixes are expected to target the current development line. Older source snapshots may not receive backports.

## Reporting a vulnerability

Do **not** publish exploit details, private crash material, credentials, or security-sensitive logs in a public issue.

Use GitHub's **Private vulnerability reporting** feature for this repository when available. If private reporting is not enabled, open a minimal public issue stating that you need a private security contact channel, without including vulnerability details.

A useful report should contain the affected commit/tag, environment, impact, reproducibility conditions, and the smallest non-sensitive proof needed to validate the problem.

## Scope

Relevant security issues include memory-safety defects, unsafe DLL loading behavior, privilege-boundary problems, malicious configuration/file parsing, and vulnerabilities introduced by the proxy/hook/runtime logic.

Game anti-cheat bans, unsupported modding policies, general Wine/Proton bugs, and vulnerabilities exclusively in an upstream dependency should normally be reported to the responsible upstream project as well.

## No bypass objective

Vulkanized-Fakenvapi is a compatibility and latency research project. Security fixes must not intentionally add anti-cheat, DRM, licensing, or platform-security bypass behavior.
