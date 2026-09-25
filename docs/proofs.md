# Proofs

Every claim in this file is a test case in this repository. Nothing is asserted
that a reader cannot reproduce with the commands shown.

## Running the proof surface

```console
cmake --preset release && cmake --build --preset release
ctest --preset release --output-on-failure
```

To inspect a single case:

```console
build/release/tests/rnf_test_property --test composition_is_arrival_order_independent
build/release/tests/rnf_test_multiprocess --test kill_after_the_commit_before_the_ack_is_recoverable
build/release/tests/rnf_test_authority --suite authority --verbose
```

Property tests take a seed from `--seed`, then `RNF_TEST_SEED`, then a fixed
default, and print the seed with every failure.

## 1. No out-of-rack resource gains rack authority

Three independent layers enforce this.

* **At ingest.** A membership claim naming a different rack is refused with
  `out_of_rack` and never enters the ledger.
  `adversarial.replayed_evidence_is_idempotent_at_the_daemon`,
  `multiprocess.cli_reports_typed_refusals_and_exit_codes`.
* **At composition.** A device observation with no matching membership claim is
  reported as `unknown_member` and excluded. A port whose owning device is not a
  member is `out_of_rack`. An obligation naming a resource outside the rack is
  `out_of_rack`.
  `compose.out_of_rack_membership_never_gains_authority`,
  `compose.device_without_membership_is_excluded`,
  `compose.port_on_foreign_device_is_refused`,
  `compose.out_of_rack_obligation_is_refused`.
* **At authority.** A scope that does not resolve inside the rack is refused
  with `not_a_member` or `out_of_rack`.
  `authority.out_of_rack_scope_is_refused`.

The tests also assert the numeric consequence: a foreign device's capacity never
appears in the rack's `total_capacity`.

## 2. Stale membership cannot authorize current use

* Ephemeral discovery evidence applies only at the generation it names; at the
  next generation the operator's sticky view applies instead, and the stale
  record is reported as a diagnostic rather than silently ignored.
  `compose.ephemeral_evidence_does_not_carry_forward`.
* Evidence naming a later generation is not applied to an earlier one.
  `compose.future_generation_evidence_is_not_applied`.
* A membership change advances the rack epoch, and a lease bound to the previous
  epoch is refused with `stale_epoch`.
  `authority.epoch_change_fences_the_lease`.
* A device that leaves and rejoins under a new incarnation changes the basis
  digest of every scope that touched it, so those leases are refused with
  `fenced_incarnation`.
  `authority.member_reincarnation_fences_the_lease`.
* A link whose capacity changed after a lease was issued fences that lease, so a
  grant can never silently exceed the capacity it was granted against.
  `authority.capacity_change_fences_the_lease`.

## 3. Incompatible exclusive grants cannot overlap

`authority.exclusive_grants_cannot_overlap` covers the rules directly: two
shared grants on the same resource coexist, an exclusive grant on a resource
that a shared grant holds is refused with `exclusive_conflict`, a disjoint
resource can be held exclusively, a shared request that reaches a resource held
exclusively is refused, and a rack-wide exclusive request conflicts with
everything.

The rule is also attacked from two independent directions:

* `property.live_grants_never_overlap_when_exclusive` drives randomised acquire
  sequences over randomised racks and then recomputes every live grant's
  footprint from the snapshot and checks pairwise that no exclusive pair
  overlaps. This does not use the occupancy index at all.
* `differential.exclusive_conflict_index_matches_pairwise_scan` compares the
  production occupancy index against an O(n*m) brute-force scan over the same
  registry state and requires them to agree on every request.
* `multiprocess.exclusive_grants_from_two_clients_do_not_overlap` races two
  independent `rnfctl` processes for the same exclusive scope and asserts that
  exactly one wins.

## 4. Capacity closes exactly

The identities are

```
total             == unavailable + usable
usable + deficit  == obligated + headroom_floor + uncommitted
```

* `property.capacity_closure_holds_for_random_racks` composes 200 randomised
  racks and re-derives both identities with checked arithmetic, and additionally
  checks that the sum of per-resource obligations is never below the ledger's
  obligated total.
* `differential.capacity_ledger_matches_reference_model` computes the ledger a
  second time from the raw evidence with a deliberately naive model that shares
  no code with the composer, and requires every field to agree.
* `compose.obligations_reduce_capacity_and_close_exactly` pins the arithmetic on
  a hand-built case.
* `compose.over_commit_is_reported_as_a_deficit_not_clamped` shows that an
  over-commit is reported as a deficit and never hidden by clamping.
* `authority.capacity_is_consumed_from_every_pool` shows a grant being charged
  to the root resource, its owning device and the rack pool, and released from
  exactly those pools.

## 5. Maintenance and drain exclusions survive stale and replayed input

`compose.maintenance_excludes_and_survives_replay` replays an older clear after
a newer drain and shows the exclusion is still in force; a genuinely newer clear
lifts it. `compose.maintenance_from_one_source_cannot_be_cleared_by_another`
shows that a clear from a second source cannot lift the first source's drain.
`compose.rack_wide_maintenance_excludes_everything` shows a rack-wide directive
makes every resource ineligible and drives usable capacity to zero.

`authority.maintenance_blocks_use_without_fencing` shows the distinction the
design depends on: maintenance makes a lease *unusable* with
`scope_not_eligible` while leaving it alive, so lifting the drain restores it.

## 6. Member reincarnation fences old authority

`authority.member_reincarnation_fences_the_lease` and
`authority.capacity_change_fences_the_lease` cover the in-process case.
`authority.stale_incarnation_token_is_refused` forges the incarnation, the fence
and the basis digest one at a time and requires a typed refusal for each.

Across processes, `multiprocess.clean_restart_fences_the_previous_incarnation`
stops a daemon cleanly, restarts it from the same store, and shows that the old
token is refused with `fenced_incarnation` while the durable grant is still
visible under a fresh token issued by the new incarnation, still reserving its
capacity.

## 7. Equivalent evidence yields equivalent canonical state

* `property.composition_is_arrival_order_independent` shuffles the evidence list
  with a seeded Fisher-Yates shuffle and requires identical ledger digests,
  evidence digests and member set digests.
* `compose.digest_is_content_addressed_and_order_independent` interleaves two
  different orders and requires byte identical snapshots, and shows that
  changing anything visible changes the digest.
* `multiprocess.equivalent_evidence_gives_an_equivalent_snapshot_across_processes`
  submits the same evidence in forward and reverse order to two different daemon
  processes with two different stores and requires the same snapshot digest.
* `compose.snapshot_encoding_roundtrips` decodes the canonical encoding back and
  requires equality, and refuses every truncation of it.

## 8. Real process kill and restart across grant, commit and ack boundaries

These tests spawn real `rnfd` and `rnfctl` processes, drive them over a real
loopback TCP socket, and terminate them with `TerminateProcess` on Windows and
`SIGKILL` on POSIX. Threads are never used as a substitute.

* `multiprocess.kill_before_the_grant_commit_leaves_no_trace` kills the daemon
  with no clean shutdown marker. On restart the rack comes up `recovering`, no
  grants exist, and a lookup for a request that was never committed answers
  `UNKNOWN`. New authority is refused until an operator moves the rack forward.
* `multiprocess.kill_after_the_commit_before_the_ack_is_recoverable` starts a
  separate `rnfctl` process that acquires a grant, kills it mid-flight, then
  kills the daemon. The test accepts either outcome and checks that whichever
  one is reported is consistent: if the lookup is remembered, the grant is
  visible under a *new* fence and the *new* incarnation; if it is not, no grant
  exists at all. It never invents a grant and never returns the old token.
* `multiprocess.kill_at_the_checkpoint_leaves_replayable_state` checkpoints,
  kills the daemon, restarts it, and requires the accepted evidence, member set
  and capacity accounting to be intact and the rack to report
  `recovered_from_dirty_shutdown`.
* `multiprocess.a_killed_client_does_not_disturb_the_daemon` kills client
  processes mid-request and requires the daemon's ledger to still close and to
  still serve.
* `integration.a_store_backed_daemon_reopens_with_the_same_state` covers the
  clean path: every component of the state, including the snapshot digest, is
  identical after a clean restart.

## 9. Persistence integrity

* `persist.torn_tail_is_discarded_and_reported` truncates the log mid-record and
  requires the incomplete record to be dropped, reported, and physically removed
  so the next append is clean.
* `persist.header_only_tail_is_treated_as_torn` covers a partial record header.
* `persist.corrupt_record_stops_replay_at_the_prefix` flips a byte inside a
  record and requires replay to stop at the preceding prefix.
* `persist.checkpoint_shortens_replay_without_losing_state` and
  `persist.corrupt_checkpoint_falls_back_to_full_replay` cover the checkpoint.
* `persist.incompatible_format_version_is_refused` and
  `adversarial.store_rejects_a_truncated_manifest` cover the refusal paths.
* `persist.a_second_open_of_the_same_directory_is_refused` covers the exclusive
  lock.

## 10. Hostile input cannot crash the runtime

* `property.evidence_decode_never_crashes_on_random_bytes` feeds 4000 random
  buffers to the evidence decoder and requires anything that decodes to
  re-encode to the same digest.
* `property.snapshot_decode_never_crashes_on_random_bytes` flips bits and
  truncates a valid snapshot 2000 times and requires that anything that decodes
  has a closing capacity ledger.
* `property.frame_header_validation_is_total` fuzzes frame headers and requires
  every accepted header to be internally consistent.
* `property.protocol_decoders_reject_random_payloads` fuzzes five message
  decoders.
* `adversarial.frame_header_rejects_every_malformed_shape` covers the specific
  malformed shapes: bad magic, wrong version, oversize length, unknown type, and
  the hard cap overriding a larger negotiated cap.
* `adversarial.hostile_frame_lengths_never_allocate` sends headers claiming
  payloads far beyond the cap and requires the daemon to answer with an error
  frame from the header alone, without reserving anything.
* `adversarial.connection_reset_mid_frame_is_a_typed_failure` promises more
  bytes than it sends and then closes; the daemon must survive and keep serving
  other clients.
* `authority.registry_bounds_are_enforced` covers the live grant bound, the
  remembered request bound and the exclusivity index bound, and checks that
  reaching a bound does not corrupt the capacity accounting.
* `adversarial.evidence_rejects_duplicate_and_extreme_identities` covers
  duplicate device/index pairs, `UINT64_MAX` capacities (checked, never
  wrapped) and maximum identities.
* `adversarial.oversize_batches_are_refused_before_allocation_grows` shows a
  batch above the configured bound is refused before it is processed.
* `adversarial.concurrent_mutation_keeps_the_invariants` runs eight threads
  against one daemon and then re-checks the capacity ledger and the exclusivity
  invariant.
* `adversarial.repeated_start_and_stop_is_stable` cycles a store-backed daemon
  six times and requires each incarnation to be distinct and the store to reopen
  cleanly.

## What is not proved

* No physical hardware, switch, ASIC, RDMA device or vendor SDK was exercised.
  Every observation in these tests is modelled evidence.
* Transport is loopback only. Multi-host behaviour is neither implemented nor
  claimed.
* AddressSanitizer coverage comes from MSVC `/fsanitize=address`. MSVC does not
  provide UndefinedBehaviorSanitizer, so that configuration was not run on this
  toolchain.
* LeakSanitizer is not available in the MSVC AddressSanitizer runtime, so the
  sanitizer runs catch memory errors but not leaks; leaks are covered instead by
  the ownership audit in `docs/concurrency-audit.md` and by the repeated
  open/close cycle tests.
