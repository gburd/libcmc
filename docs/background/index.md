---
title: Background
nav_order: 4
has_children: true
permalink: /background/
lede: >-
  The research libcmc grew out of, the review that hardened it, and its calibration.
---

The survey that led to libcmc's design, the adversarial review of its
actor mode with the fix for each finding, and its calibration against
TLC, all from 2026-11-02.  The current design is
[Design]({{ '/reference/design/' | relative_url }}).

- [Survey and original design]({{ '/background/survey/' | relative_url }})
- [Adversarial review of actor mode]({{ '/background/review/' | relative_url }})
- [Calibration against TLC]({{ '/background/calibration/' | relative_url }})
- [Process mode M0]({{ '/background/process-mode-m0/' | relative_url }}) -- a prototype toward checking code written as it ships
- [Process mode M1]({{ '/background/process-mode-m1/' | relative_url }}) -- canonical state and a real search over interleavings
- [Process mode M2]({{ '/background/process-mode-m2/' | relative_url }}) -- a real malloc heap, and the boundary it draws
- [Process mode M3]({{ '/background/process-mode-m3/' | relative_url }}) -- a model file drives the checker
- [Process mode M4]({{ '/background/process-mode-m4/' | relative_url }}) -- multi-node, the checker owns the network
- [Decision: one engine, a model file, the shipping binary]({{ '/background/decision-model-file/' | relative_url }})
