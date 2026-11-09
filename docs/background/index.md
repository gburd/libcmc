---
title: History and provenance
nav_exclude: true
has_children: true
permalink: /background/
lede: >-
  The research libcmc grew out of, the review that hardened it, its calibration, and the process-mode lab notebook.
---

This is a lab notebook, not a roadmap: the survey that led to libcmc's
design, the adversarial review of its actor mode with the fix for each
finding, the calibration against TLC, and the process-mode milestones as
they were built.  Every milestone below is done and shipped; the pages
are dated records of work already in the engine.  For the tool as it is,
start at [Design]({{ '/reference/design/' | relative_url }}),
[the model file]({{ '/reference/model-file/' | relative_url }}) and the
[examples]({{ '/examples/' | relative_url }}).

- [Survey and original design]({{ '/background/survey/' | relative_url }})
- [Adversarial review of actor mode]({{ '/background/review/' | relative_url }})
- [Calibration against TLC]({{ '/background/calibration/' | relative_url }})
- [Process mode M0]({{ '/background/process-mode-m0/' | relative_url }}) -- a prototype toward checking code written as it ships
- [Process mode M1]({{ '/background/process-mode-m1/' | relative_url }}) -- canonical state and a real search over interleavings
- [Process mode M2]({{ '/background/process-mode-m2/' | relative_url }}) -- a real malloc heap, and the boundary it draws
- [Process mode M3]({{ '/background/process-mode-m3/' | relative_url }}) -- a model file drives the checker
- [Process mode M4]({{ '/background/process-mode-m4/' | relative_url }}) -- multi-node, the checker owns the network
- [Process mode M5]({{ '/background/process-mode-m5/' | relative_url }}) -- time is an environment action (leader election)
- [Process mode M5b]({{ '/background/process-mode-m5b/' | relative_url }}) -- absolute time is engine-controlled (stale lease)
- [Process mode M6]({{ '/background/process-mode-m6/' | relative_url }}) -- crash loses volatile, keeps durable (lost commit)
- [Process mode M7]({{ '/background/process-mode-m7/' | relative_url }}) -- honest verdicts, and what Paxos costs
- [Decision: one engine, a model file, the shipping binary]({{ '/background/decision-model-file/' | relative_url }})
