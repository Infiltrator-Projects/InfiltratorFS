# Validation

## Evidence model

Validation is layered. Compilation proves source compatibility; automated tests prove their covered contracts; integration and hardware tests prove only the environments actually exercised.

## Automated gates

The executable authority for the current automatic gate set is the workflow
configuration under `.github/workflows/`. This document intentionally does not
copy an exhaustive workflow filename list, because such a list becomes a second
source of truth as qualification is added, retired or reorganised.

The automatic suite currently spans portable build/conformance, policy and
static-analysis gates, native Linux module qualification, metadata and
root-volume qualification, resize, desktop/package integration, Windows bridge
source qualification, exact release-candidate construction and immutable
release publication. The tests/ tree covers format conformance,
allocation/index trees, compression, inline/sparse/large files, fsck policy,
desktop integration and portable hardening. Environment-specific workflows add
mounted kernel, resize, root-volume, formatter and heavier qualification where
that environment is actually available.

## Manual/environment-dependent evidence

Root boot, forced interruption/power-loss scenarios, physical partition tests and the heaviest scale/endurance workloads remain separate evidence classes because ordinary CI cannot safely or economically reproduce them.

A simulated, fixture-driven or hosted result must not be described as proof of a physical-device, destructive-media or boot-path result.

## Release criterion

The exact release revision must pass its required gates, and generated assets must correspond to that revision. Known unsupported or failing behaviour remains documented as such.

## Regression rule

Reproducible defects should gain permanent regression coverage at the narrowest layer that captures the original failure. Validation documentation should distinguish automatic release blockers from optional, manual or milestone evidence.

Optimized qualification retains executable checks in the desktop/presentation regression paths. These checks report a failing expression and terminate unsuccessfully independently of `NDEBUG`; Release compilation cannot silently turn the suite into a successful no-op.

Publication is owned by tools/release/publish.sh. It repairs only drafts, compares qualified assets before and after publishing, and re-reads durable state after an ambiguous creation response. Public tags and bytes remain immutable.
