# Rack Network Fabric

Rack Network Fabric (RNF) is a C++20 runtime that answers one question for one
physical rack:

> Given exact rack membership, topology, evidence, capacity, failures,
> maintenance state and current generations, what network state is authoritative
> inside this rack, which resources and paths may be used now, and who may change
> that answer?

It is a library, a daemon (`rnfd`) and a client (`rnfctl`). It owns **rack-scoped
composition and authority**: turning accepted evidence into an immutable,
content-addressed snapshot, deciding which resources and paths are usable, and
issuing, fencing and accounting for the leases that let a principal use them.

It does **not** do physical discovery, generic routing, low-level link truth,
switch programming, global bandwidth allocation, or cross-rack/pod/cluster
governance. See "Boundaries" below for exactly where the edge is.

```
  evidence sources            Rack Network Fabric                    consumers
  ------------------          -------------------                    ---------
  operator configuration      accepted evidence ledger               rnfctl
  discovery snapshots   -->   canonical snapshot  (content hash) --> client library
  imported obligations        rack epoch / generation               embedders
  synthetic (tests)           grants, leases, fencing tokens
                              versioned, checksummed store
```

## Status

Version 1.0.0. Everything described in this README is implemented, built and
covered by the test suite in this repository. Nothing here is aspirational.

* Builds warning-clean with MSVC `/W4 /WX` in both Release and Debug.
* 11 test executables, 120 test cases, all passing in Release, Debug and with
  AddressSanitizer enabled.
* Distributed behaviour is proved with **independent operating system
  processes** over **loopback TCP**, including hard kills at different lifecycle
  boundaries.
* No third-party dependencies. No telemetry. No network traffic to anything but
  the loopback endpoint the operator configures.

## Build

Requirements: CMake 3.25 or newer, a C++20 compiler (MSVC 19.36+ or GCC 12+ or
Clang 15+), and Ninja or another generator.

On Windows, `tools/devshell.cmd` imports the Visual Studio x64 developer
environment and forwards its arguments, which is what the commands below assume.

```console
:: Windows
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
```

```console
# Linux and macOS
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

Presets: `release`, `debug`, `asan` (AddressSanitizer), `bench`
(Release plus the benchmark executable). Useful cache options:

| Option | Default | Meaning |
| --- | --- | --- |
| `RNF_BUILD_APPS` | ON | build `rnfd` and `rnfctl` |
| `RNF_BUILD_TESTS` | ON | build the test suite |
| `RNF_BUILD_EXAMPLES` | ON | build the embedding example |
| `RNF_BUILD_BENCH` | OFF | build the benchmark executable |
| `RNF_WARNINGS_AS_ERRORS` | ON | `/WX` or `-Werror` |
| `RNF_ENABLE_ASAN` | OFF | AddressSanitizer (MSVC, GCC, Clang) |
| `RNF_ENABLE_UBSAN` | OFF | UndefinedBehaviorSanitizer (GCC/Clang only) |

Tests never carry a CTest `TIMEOUT`, and no test uses a watchdog. A test that
hangs is treated as a defect in the runtime or the test, not as something to cut
short.

## Quick start

Start a daemon for rack 7 with durable state in `./rack7`:

```console
rnfd --rack 7 --name rack-7 --store ./rack7 --port 8790 --auto-compose
```

It prints a single readiness line and then serves loopback only:

```
RNFD READY port=8790 rack=0000000000000007 incarnation=1 generation=0 epoch=0 lifecycle=assembling pid=12345
```

Declare a two device rack, bring it active, take a lease, and inspect it:

```console
rnfctl --endpoint 127.0.0.1:8790 declare-member --device 1 --incarnation 1
rnfctl --endpoint 127.0.0.1:8790 declare-device --device 1 --role switch --capacity 1000 --availability up
rnfctl --endpoint 127.0.0.1:8790 declare-port  --port 10 --device 1 --index 0 --port-role uplink --capacity 400
rnfctl --endpoint 127.0.0.1:8790 declare-port  --port 20 --device 1 --index 1 --port-role access --capacity 400
rnfctl --endpoint 127.0.0.1:8790 declare-link  --link 100 --port-a 10 --port-b 20 --capacity 400
rnfctl --endpoint 127.0.0.1:8790 lifecycle --state active
rnfctl --endpoint 127.0.0.1:8790 state
rnfctl --endpoint 127.0.0.1:8790 acquire --request 00000000000000000000000000000001 \
        --principal 42 --scope device:1 --capacity 250 --ttl-ms 60000
rnfctl --endpoint 127.0.0.1:8790 resources --limit 20
rnfctl --endpoint 127.0.0.1:8790 paths
rnfctl --endpoint 127.0.0.1:8790 shutdown
```

Every command prints one line of JSON. Refusals are typed: the output carries
`class`, `code` and `detail`, and the process exit code is non-zero.

Embedding the library is a pure function call:

```cpp
#include <rnf/compose/composer.hpp>

rnf::EvidenceLedger ledger;
// ... insert accepted evidence ...
rnf::RackConfig config;
config.rack = rnf::RackId(7);
rnf::ComposeInput input;
input.generation = rnf::TopologyGeneration{1};
input.lifecycle = rnf::LifecycleState::kActive;

rnf::Result<rnf::ComposeOutcome> outcome = rnf::compose(ledger, config, input);
if (outcome.has_value()) {
    std::printf("%s\n", outcome->snapshot.digest().to_hex().c_str());
}
```

See `examples/embed_fabric.cpp` for a complete program.

## Concepts

**Evidence is the only input.** A record carries its kind, its source identity,
that source's own sequence number, the topology generation it was observed
against, and how long it stays applicable. Two durability classes exist:

* `sticky` observations carry forward to every later generation. Operator
  configuration, maintenance directives and imported obligations are sticky.
* `ephemeral` observations speak only about the generation they name. A
  discovery snapshot from generation 7 says nothing about generation 8.

**The ledger is arrival-order independent.** Records are keyed by
(kind, subject, source). The stored record for a slot is the one with the
highest (generation, sequence); identical content at the same rank is
idempotent, and differing content at the same rank is resolved by the
lexicographically smaller content hash and reported as a conflict. Consequently
the canonical digest of the accepted evidence set depends only on the *set* of
records, never on the order they arrived in.

**A snapshot is immutable, canonical and content-addressed.** Every collection
inside it is sorted by a strongly typed identity, the encoding is fixed-width
little-endian with no padding and no undefined bytes, and the content address is
a BLAKE2s-256 digest over that encoding. Equivalent evidence produces byte
identical snapshots and therefore the same digest.

**Authority is separate from observation.** Observing a link does not grant the
right to use it. A grant (a lease) is issued by the daemon and binds:

* the rack identity,
* the topology generation it was issued at,
* the rack **epoch** (which advances exactly when the member set changes),
* the **controller incarnation** (which strictly increases on every daemon
  start and every revalidation),
* the resource scope,
* a fencing token, and
* a digest of the member incarnations and capacities the scope depends on.

Change any of those and the lease is fenced with a typed refusal
(`stale_epoch`, `fenced_incarnation`, `stale_generation`) rather than silently
extended.

**Lifecycle.** `assembling -> active -> degraded -> draining -> maintenance`,
with `recovering` after an unclean restart, and `retired` terminal. New
authority is only issued in `active` and `degraded`. Maintenance excludes
resources from new grants without destroying existing leases; reincarnation and
epoch changes fence them.

**Capacity closes exactly.** The ledger satisfies, with checked arithmetic:

```
total      == unavailable + usable
usable + deficit == obligated + headroom_floor + uncommitted
```

A deficit is reported, never clamped away. Missing capacity evidence is
`UNKNOWN`, never zero and never "enough".

## Guarantees and how to reproduce them

Every claim below is a named test case. Run the whole surface with
`ctest --preset release --output-on-failure`.

| Claim | Test |
| --- | --- |
| No out-of-rack resource gains rack authority | `compose.out_of_rack_membership_never_gains_authority`, `compose.device_without_membership_is_excluded`, `authority.out_of_rack_scope_is_refused`, `adversarial.replayed_evidence_is_idempotent_at_the_daemon` |
| Stale membership cannot authorize current use | `compose.ephemeral_evidence_does_not_carry_forward`, `compose.future_generation_evidence_is_not_applied`, `authority.epoch_change_fences_the_lease` |
| Incompatible exclusive grants cannot overlap | `authority.exclusive_grants_cannot_overlap`, `property.live_grants_never_overlap_when_exclusive`, `differential.exclusive_conflict_index_matches_pairwise_scan`, `multiprocess.exclusive_grants_from_two_clients_do_not_overlap` |
| Capacity closes exactly | `property.capacity_closure_holds_for_random_racks`, `compose.obligations_reduce_capacity_and_close_exactly`, `compose.over_commit_is_reported_as_a_deficit_not_clamped`, `differential.capacity_ledger_matches_reference_model` |
| Maintenance and drain exclusions survive stale and replayed input | `compose.maintenance_excludes_and_survives_replay`, `compose.maintenance_from_one_source_cannot_be_cleared_by_another` |
| Member reincarnation fences old authority | `authority.member_reincarnation_fences_the_lease`, `authority.capacity_change_fences_the_lease`, `multiprocess.clean_restart_fences_the_previous_incarnation` |
| Equivalent evidence yields equivalent canonical state | `property.composition_is_arrival_order_independent`, `compose.digest_is_content_addressed_and_order_independent`, `multiprocess.equivalent_evidence_gives_an_equivalent_snapshot_across_processes` |
| Kill and restart across grant, commit and ack boundaries | `multiprocess.kill_before_the_grant_commit_leaves_no_trace`, `multiprocess.kill_after_the_commit_before_the_ack_is_recoverable`, `multiprocess.kill_at_the_checkpoint_leaves_replayable_state` |

The multiprocess tests spawn real `rnfd` and `rnfctl` processes, drive them over
a real loopback TCP socket, and terminate them with `TerminateProcess` /
`SIGKILL`. Threads are never used as a substitute for that proof.

## Persistence and recovery

State lives in a directory:

```
rnf.lock        exclusive lock for the lifetime of the process
rnf.meta        fixed size manifest: format version, rack binding, incarnation
                counter, checkpoint offset and digests
rnf.checkpoint  accepted evidence plus the composed snapshot at that offset
rnf.wal         append-only, length prefixed, CRC-32C protected records
```

* Every record application is idempotent, so replaying a prefix of the log
  twice is safe. That is what makes a crash between the checkpoint rename and
  the manifest update harmless.
* A torn record at the end of the log is discarded and reported
  (`truncated_tail`); replay stops at the last record whose checksum verifies.
* A record that fails its checksum in the middle of the log stops replay at the
  preceding prefix and is reported as `corrupt_record`.
* A store written by a different format version is **refused**, never
  reinterpreted.
* A damaged manifest is **fatal by default**, because it holds the only durable
  copy of the process incarnation counter. `StoreOptions::allow_manifest_loss`
  opts into rebuilding it from the log.
* The incarnation counter strictly increases across restarts of the same store,
  which is what fences authority issued by a dead process.
* Recovered dynamic evidence is historical: a grant recovered from durable state
  lands in `recovering` and reserves its capacity, but reports
  `INDETERMINATE` until it has been revalidated against a fresh snapshot under
  the new incarnation.

## Install and consume

```console
cmake --install build/release --prefix /opt/rnf
```

```cmake
find_package(rnf 1.0 CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE RNF::rnf)
```

`examples/downstream` is a complete, independent consumer project that is
deliberately not part of this build. It is the check that the installed prefix
is genuinely usable on its own:

```console
cmake -S examples/downstream -B /tmp/rnf-consumer -DCMAKE_PREFIX_PATH=/opt/rnf
cmake --build /tmp/rnf-consumer
/tmp/rnf-consumer/rnf_downstream
```

The library is built as a static archive. That is a deliberate choice: it keeps
the exported package free of ABI-sensitive DLL export macros and lets a consumer
link the runtime into a single self-contained binary.

## Tooling

`rnfctl` covers inspection (`state`, `resources`, `paths`, `grants`, `stats`),
control (`compose`, `lifecycle`, `checkpoint`, `shutdown`), evidence declaration
for every record kind, and the full lease lifecycle (`acquire`, `renew`,
`release`, `validate`, `lookup`).

`rnf_bench` measures completed work only: hashing, codec round trips, ledger
insertion, composition at three rack sizes, grant acquire/release, durable and
buffered log appends, and end-to-end daemon round trips over loopback TCP. It
prints the median of N repetitions together with the operation count, so every
figure can be reproduced or contradicted. Build it with
`cmake --preset bench && cmake --build --preset bench`.

## Testing

```console
ctest --preset release --output-on-failure     # the whole surface
ctest --preset debug   --output-on-failure     # same suite, checked iterators
ctest --preset asan    --output-on-failure     # AddressSanitizer
build/release/tests/rnf_test_property --seed 12345
build/release/tests/rnf_test_multiprocess --test kill_after_the_commit_before_the_ack_is_recoverable
```

Property tests are driven by a deterministic generator. The seed comes from
`--seed`, then `RNF_TEST_SEED`, then a fixed default, and every failure prints
the seed that produced it.

The suite covers unit behaviour, seeded properties, differential comparison
against deliberately naive reference models, adversarial input (malformed
framing, truncation, corruption, oversize counts, extreme values, duplicate
identities, replayed events, invalid UTF-8), concurrency and ownership hazards,
an in-process TCP integration surface, and the multiprocess kill/restart proofs.

## Boundaries

RNF is rack scoped by construction, not by convention. A membership claim that
names another rack is refused at ingest. A device observation with no matching
membership claim never becomes authoritative. An imported obligation that names
a resource outside the rack is reported as `out_of_rack` and excluded. A scope
that does not resolve inside the rack is refused with `not_a_member` or
`out_of_rack`.

Adjacent runtimes own the rest, and RNF assumes they exist:

| Neighbour | Owns | RNF's relationship |
| --- | --- | --- |
| Physical discovery | What hardware exists and what the links really are | Supplies evidence records; RNF composes, it does not probe |
| Switch / ASIC programming | Turning intent into forwarding state | Consumes RNF leases; RNF never programs a device |
| Global bandwidth allocation | Fairness across racks and pods | Supplies imported obligations; RNF applies them as in-rack reservations |
| Routing | Path computation across the fabric | RNF enumerates simple in-rack paths only |
| Cross-rack governance | Pod and cluster policy | Supplies epoch-independent constraints only |

## Limitations

Stated plainly, because a runtime that overstates its own coverage is worse than
one that does not exist.

* **No hardware was involved.** Every test runs against modelled evidence. No
  switch, NIC, ASIC, RDMA device or vendor SDK was exercised, so no claim is
  made about physical network effects, forwarding behaviour or line rate.
* **Loopback only.** The transport binds `127.0.0.1`. Binding any other address
  is refused with `unsupported`. Multi-host operation is not implemented and is
  not claimed.
* **Path enumeration is simple and bounded.** It finds short simple chains of
  eligible links between an access port and an uplink, fabric or peer port, with
  hop and count limits, and reports truncation. It is not a routing algorithm.
* **UndefinedBehaviorSanitizer is unavailable on MSVC**, so the UBSan
  configuration is only buildable with GCC or Clang. The sanitizer results
  reported for this repository come from MSVC AddressSanitizer.
* **Capacity units are abstract.** RNF never interprets them; the operator
  decides what a unit means and must use one unit per rack.
* **Time is only ever compared.** Lease deadlines use wall-clock milliseconds;
  RNF does not assume a monotonic clock across processes.
* **A store is bound to one rack.** Opening a store that belongs to a different
  rack is refused rather than merged.
* **Grant capacity is charged to the root resource, its owning devices and the
  rack pool.** An obligation that targets a device therefore reduces both the
  device pool and the rack pool. That is conservative on purpose and is
  documented in `capacity_pool_refs`.

## Documentation

* `docs/architecture.md` - modules, data flow and the composition algorithm
* `docs/protocol.md` - framing, message set and bounds
* `docs/operations.md` - running, inspecting and recovering a rack
* `docs/proofs.md` - the invariant list, the test that proves each one, and how
  to reproduce the multiprocess kills
* `docs/concurrency-audit.md` - the explicit lock, ownership and lifetime audit
* `docs/benchmarks.md` - measured figures and the exact command that produced them

## Contributing

See `CONTRIBUTING.md`. Contributions are accepted under the Apache License 2.0
with no Contributor License Agreement to sign.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
