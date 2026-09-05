# Security policy

PulseFanout is an experimental transport. Raw UDP mode requires a trusted network boundary; secure mode uses the mutually authenticated TLS relay described below.

## Reporting a vulnerability

Please use GitHub's private vulnerability reporting or a private security advisory for this repository. Do not open a public issue containing exploit details, credentials, or sensitive deployment information.

Include:

- the affected commit or release;
- a minimal reproduction;
- the expected and observed impact;
- any suggested mitigation.

## Security boundaries

The receiver validates frame lengths, message metadata, and bounded FEC envelopes before shared-memory publication. These checks do not provide authentication, confidentiality, replay protection, or denial-of-service protection. Operators remain responsible for network isolation, host access control, and resource limits.

## Transport modes

Raw UDP requires explicit --allow-insecure-udp and remains unauthenticated. For untrusted remote links use the mutually authenticated TLS 1.3 relay with Unix-stream endpoints, described in [transport operations](docs/transport.md). Certificate identities are verified in both directions. Durable relay mode persists accepted frames and replays pending entries. This is at-least-once delivery, not exactly-once application processing. Shared-memory overload is bounded and fails explicitly by default.
