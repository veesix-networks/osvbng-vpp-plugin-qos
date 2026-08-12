# Codex review: spec compliance

- **Reviewer:** Codex (gpt-5 via codex-cli 0.144.6, `/codex:adversarial-review`)
- **Lens:** did the implementation build what `IMPLEMENTATION_SPEC.md` (as
  amended by PHASE5_FINDINGS.md F5-2/F5-3/F5-4) specifies; VPP binary API
  correctness
- **Target:** branch diff `c90ebf6..HEAD`
- **Date:** 2026-08-12
- **Verdict as returned:** needs-attention

Output below is verbatim; triage with accept/reject rationale lives in
`../DECISIONS.md`.

---

No-ship: malformed v2 requests can delete the port tier, port updates can
violate the required parent/child rate invariant, and the mandated cache-line
layout was silently changed.

Findings:

- **[high] Invalid aggregate level is treated as a port operation
  (src/osvbng_qos_sched_api.c:232-261)**
  The create/delete handlers branch only on SVLAN; every other value,
  including invalid enum values, executes the port path. A v2 delete with
  level=2 therefore deletes the port aggregate rather than returning
  INVALID_VALUE, detaching all direct schedulers when no S-VLAN children
  exist. Update has the same aliasing through cake_aggregate_update.
  Recommendation: Explicitly accept only PORT and SVLAN in all three
  handlers/core functions and reject every other value before mutation.

- **[high] Port rate updates can leave children configured above their parent
  (src/osvbng_qos_sched.c:831-850)**
  The child<=parent check runs only when updating an aggregate that already
  has a parent. Updating a port to a rate below an existing S-VLAN therefore
  succeeds, violating the spec's hierarchy invariant and leaving an invalid
  configuration that the create path would reject.
  Recommendation: Before lowering a port rate, walk its S-VLAN children under
  the barrier and reject the update if any child rate exceeds the proposed
  rate.

- **[medium] Aggregate cache-line layout contradicts section 4.7 without an
  accepted amendment (src/osvbng_qos_sched.h:220-230)**
  Section 4.7 requires drr.effective_weight on read-mostly cacheline0 and
  only round_deficit on cacheline3. The implementation deliberately places
  the entire drr object, including effective_weight, on cacheline3. This is
  a direct, unrecorded specification deviation; PHASE5_FINDINGS.md does not
  amend the layout requirement.
  Recommendation: Either split effective_weight into cacheline0 as specified,
  or document and approve a PHASE5 finding that formally amends section 4.7
  with supporting contention measurements.

Next steps:
- Fix invalid-level rejection and port-rate invariant enforcement.
- Resolve the section 4.7 layout deviation before shipment.
- Add negative API tests for out-of-range levels and lowering a port beneath
  a child rate.
