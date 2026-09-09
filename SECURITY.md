# Security Policy

## Supported versions

Until the first stable release, only the current development branch is expected to receive security fixes.

## Reporting a vulnerability

Please do **not** open a public issue for a security vulnerability that may enable:

- arbitrary code execution,
- unsafe DLL / shared-library loading,
- privilege escalation,
- path traversal,
- malicious configuration execution,
- memory corruption,
- unsafe handling of untrusted game data.

Use GitHub's private security advisory / private vulnerability reporting feature if it is enabled for this
repository.

If private reporting is not enabled, contact the repository owner privately through a published maintainer
contact channel before disclosing technical details publicly.

Include:

- affected commit / release,
- reproduction steps,
- expected and observed behavior,
- impact,
- proof of concept if appropriate,
- suggested mitigation if known.

## Anti-cheat / online-game warning

Vulkanized-Fakenvapi is intended for compatibility and experimentation. Injected or substituted libraries can
be incompatible with anti-cheat systems. Do not assume that a technically functional setup is permitted by a
game's terms or safe for an online account.
