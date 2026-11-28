# 0071: Field-graph precision rules, from SP-06

Date: 2026-11-27

## Status

Accepted. Amends the field-graph evaluators of [0052](0052-procgen-runtime.md). Validated by SP-06 (VCH-082,
[findings](../research/spikes/sp-06-field-graph.md)).

## Context

SP-06 evaluated one terrain field graph (8 nodes, 11 noise octaves per sample, JSON IR with stable node ids and
versioned node types) three ways over a 1024² cube-sphere face: the canonical CPU batch interpreter (20.1 M samples/s
per core on the M5 Max), a GPU interpreter in Slang (0.282 ms per 10⁶ samples) and a hand-written Slang kernel
(0.221 ms): 1.28× for interpreting, well under ADR 0052's 2× bar, so graphs stay interpreted and runtime Slang
compilation (VCH-009) is not needed for procgen. The three agree within 5.7e-7.

They agreed only after one fix. GPU shaders compile with fast math (visual tier), which fuses multiply-adds; two
expressions in gradient noise (the lattice-cell fraction and the corner dot product) lose precision badly when fused
on one evaluator and not the other: 9.7e-6 on this graph, 9e-4 on one high-frequency octave.

## Decision

1. **Precision-critical products are written as explicit fused operations in the node contract.** A node's reference
   implementation names them, and every evaluator computes them fused, once:
   - the canonical CPU evaluator with `std::fma` (exactly rounded on every platform; allowed by ADR 0043 rule 1);
   - Metal with `fma`;
   - DX12 with `mad`, which D3D12 does not guarantee fused for floats (exact `fma` needs double-precision shader
     support); GPU output is visual-only, and the WARP agreement test sets the DX12 tolerance.
   Everything else in a node stays plain arithmetic, which the CPU never contracts (ADR 0043).
2. **Lattice hashing:** node and octave seeds come from `veach::hash` ([0044](0044-counter-based-hash-randomness.md));
   each lattice corner uses a 32-bit hash of those seeds and the corner's integer coordinates. This is frozen with
   `veach:fbm@1`; a different corner hash is a new node version.
3. **The CPU interpreter evaluates in batches with inlined noise kernels**, bit-equal to evaluating one sample at a
   time; that equality is tested, so batching never changes canonical output.

## Consequences

- CPU (canonical: colliders, placement, servers) and GPU (visual) terrain agree within 1e-6 on Metal, so what you see
  is what you stand on; SP-12 measures the remaining difference in millimeters.
- Node authors must mark fused products; a missing mark shows up as a CPU-GPU disagreement in the node's agreement test.
- DX12 visual terrain may differ slightly more than Metal's; it is never used for gameplay.

## Alternatives considered

- **Turn off fast math for procgen shaders:** the GPU loses its speed for a visual-tier result, and Metal could still
  fuse in other places.
- **Never fuse on any evaluator:** the GPU compiler fuses anyway under fast math, so the disagreement remains.
- **A 64-bit hash per lattice corner:** higher quality on paper, assumed too slow in the CPU's inner loop on ARM
  (not measured); revisit with a new node version if a quality problem shows.
