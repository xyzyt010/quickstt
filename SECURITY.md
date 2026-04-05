# Security Policy

## Supported Branch

The most recent maintained branch and tagged release should be considered the supported security target.

## Reporting a Vulnerability

Please report security issues privately before public disclosure.

Include:
- affected version
- exact reproduction details
- whether credentials, local files, or remote execution are involved
- whether the issue is local-only, LAN, or internet-facing

## Security Priorities

QuickSTT handles:
- local microphone access
- local text injection
- API keys and cloud credentials
- optional network-based update and package distribution
- local device-control credentials

The project treats the following as high priority:
- credential leakage
- unsigned or untrusted update execution
- arbitrary file overwrite during update/install
- remote code execution paths
- unsafe smart-home control execution

## Current Hardening Direction

- least-surprise local behavior
- explicit firewall enablement
- safer secret storage on Windows
- clearer release packaging and verification
- future signed release assets and published checksums

