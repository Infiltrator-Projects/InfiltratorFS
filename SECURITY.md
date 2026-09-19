# Security

## Scope

Security-relevant issues include memory or bounds errors, malformed-input handling, privilege-boundary mistakes, unsafe path/file handling, destructive-operation guard failures, unsafe device/media writes, insecure loading and persistent-data corruption triggered by untrusted input.

## Reporting

Do not publish exploit details in a public issue. Use GitHub private vulnerability reporting/security advisories for the repository when available.

Provide the affected revision, environment, reproduction, expected/observed behaviour and known impact boundary.

## Response and validation

Treat security defects as correctness defects. Reproduce, add a regression test where practical, fix the underlying contract and validate the affected platform or persistence boundary. Do not claim broader proof than the test environment provides.

## Supported source

Current main and the current released line are the primary maintained sources unless explicitly documented otherwise.
