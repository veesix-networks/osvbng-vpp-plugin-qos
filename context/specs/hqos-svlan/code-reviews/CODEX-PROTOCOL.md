# Codex review: protocol conformance

- **Reviewer:** Codex (gpt-5 via codex-cli 0.144.6, `/codex:adversarial-review`)
- **Lens:** algorithm correctness against the DRR literature (Shreedhar &
  Varghese, SIGCOMM '95) and the design's own claims: quantum/deficit
  arithmetic, round-tag wraparound, effective-quantum derivation, the charged
  work-conserving escape, hierarchical composition, shaper fixed-point
  arithmetic, atomic orderings
- **Target:** branch diff `ae8ed7c..HEAD`
- **Date:** 2026-08-12
- **Verdict as returned:** needs-attention

PROCESS.md assigns this lens to Gemini; it was run as a second Codex pass at
the user's direction. Output below is verbatim; triage lives in
`../DECISIONS.md`.

---

Do not ship: long-idle children can stall for weeks, and high-rate shaping
systematically exceeds configured rates.

Findings:

- **[high] Signed round comparison blocks children after half the u32 round
  space (src/cake_drr.h:78-100)**
  The signed-difference test is valid only when tags differ by less than
  2^31. A child last serviced over 2^31 ms (~24.9 days) ago sees
  `(i32)(round-current) <= 0`, so it receives no refill. Reactivation
  preserves its old round. The escape admits packets only until debt crosses
  -2048 bytes; e.g. two 1500-byte packets produce -3000, after which the
  child remains blocked until the modular difference re-enters the positive
  half-space—potentially another ~24.9 days. Newly initialized shared
  children can hit the same failure when created after 24.9–49.7 days uptime
  because their tag starts at zero.
  Recommendation: Initialize round tags from the current round and explicitly
  rebase inactive/stale children on activation. Do not rely solely on modular
  signed comparison when a tag may be stale for >=2^31 rounds.

- **[medium] Per-packet cost truncation materially overspeeds high-rate
  shapers (src/cake_shaper.h:31-43)**
  `cake_cost_ns` floors every packet independently and discards the
  fractional remainder. At 100 Gbit/s (12.5 GB/s), the scaled rate is 5242
  and a 64-byte charge costs 5 ns instead of 5.12 ns, permitting
  102.4 Gbit/s—2.4% above configuration. At 40 Gbit/s, 64 bytes cost 12 ns
  instead of 12.8 ns, permitting about 42.67 Gbit/s. The error accumulates
  rather than averaging out.
  Recommendation: Preserve fractional virtual time in the gate, or round
  packet cost upward with a checked 128-bit multiply. Add sustained
  minimum-frame tests at 40 and 100 Gbit/s.

Next steps:
- Fix both arithmetic counterexamples and add deterministic regression tests
  before shipping.
