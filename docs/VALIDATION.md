# Validation

## Evidence model

Validation is layered. Compilation proves source compatibility; automated tests prove their covered contracts; integration and hardware tests prove only the environments actually exercised.

## Automated gates

- .github/workflows/ci.yml
- .github/workflows/kernel-module.yml
- .github/workflows/resize-qualification.yml
- .github/workflows/root-boot-qualification.yml
- .github/workflows/root-volume-qualification.yml
- .github/workflows/heavy-qualification.yml
- .github/workflows/desktop-formatter-qualification.yml
- .github/workflows/release-packages.yml

The tests/ tree covers format conformance, allocation/index trees, compression, inline/sparse/large files, fsck policy, desktop integration and portable hardening. Dedicated workflows add mounted kernel, resize, root-volume, formatter and heavy qualification.

## Manual/environment-dependent evidence

Root boot, forced interruption/power-loss scenarios, physical partition tests and the heaviest scale/endurance workloads remain separate evidence classes because ordinary CI cannot safely or economically reproduce them.

A simulated, fixture-driven or hosted result must not be described as proof of a physical-device, destructive-media or boot-path result.

## Release criterion

The exact release revision must pass its required gates, and generated assets must correspond to that revision. Known unsupported or failing behaviour remains documented as such.

## Regression rule

Reproducible defects should gain permanent regression coverage at the narrowest layer that captures the original failure. Validation documentation should distinguish automatic release blockers from optional, manual or milestone evidence.
