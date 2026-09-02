# Security policy

PulseFanout is currently an experimental transport and should be deployed only inside a trusted network boundary.

## Reporting a vulnerability

Please use GitHub's private vulnerability reporting or a private security advisory for this repository. Do not open a public issue containing exploit details, credentials, or sensitive deployment information.

Include:

- the affected commit or release;
- a minimal reproduction;
- the expected and observed impact;
- any suggested mitigation.

## Security boundaries

The receiver validates frame lengths, message metadata, and bounded FEC envelopes before shared-memory publication. These checks do not provide authentication, confidentiality, replay protection, or denial-of-service protection. Operators remain responsible for network isolation, host access control, and resource limits.
