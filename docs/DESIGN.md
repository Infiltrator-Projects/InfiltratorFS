# Design

## First-principles position

InfiltratorFS starts with the behaviour the project must own. Standards, platform frameworks and mature implementations are evidence and mechanisms, not specifications to copy blindly or semantic dependencies that may redefine the product later.

## Goals

- preserve crash-consistent atomic publication and recoverability
- keep format semantics independent of any one operating system
- make retained history, integrity and allocation rules explicit
- qualify real mounted/boot behaviour separately from portable tests

## Non-goals

Pre-1.0 development does not promise compatibility with obsolete development formats. A portable test pass is not evidence that a native mounted or root-boot path has passed.

## Dependency and language policy

Prefer first-party C/C++ for portable/native implementation where it fits the problem. Use platform-native language/frameworks at genuine platform boundaries. Dependencies are accepted when their documented contract is stronger than reimplementation, but project-owned behaviour stays explicit and testable.

## Failure semantics

Unknown, unavailable, unsupported and invalid are distinct states. The project prefers a clear refusal to guessed success. Mutating or destructive operations require stronger preconditions and post-verification than read-only operations.

## Decision quality

A design change should state the problem, alternatives, evidence, trade-offs and validation method. Newness alone is not a benefit. Proven mechanisms remain when they are the strongest justified choice.
