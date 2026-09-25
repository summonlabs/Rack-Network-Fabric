# Architecture

This document describes what the code actually does. Every section names the
files that implement it.

## Layers

```
include/rnf/                     source/
  core/        primitives          src/core/
    status.hpp   typed outcomes      status.cpp
    id.hpp       strong ids          id.cpp
    hash.hpp     BLAKE2s, CRC-32C    hash.cpp
    bytes.hpp    canonical codec     bytes.cpp
    checked.hpp  checked arithmetic
    log.hpp      local logging       log.cpp
  model/       rack vocabulary      src/model/
    types.hpp    roles, generations  types.cpp
    evidence.hpp records, ledger     evidence.cpp
  compose/     state composition    src/compose/
    snapshot.hpp canonical snapshot  snapshot.cpp
    composer.hpp pure composer       composer.cpp, scope.cpp
  authority/   leases               src/authority/
    grant.hpp    scope binding       grant.cpp
    registry.hpp grant registry      registry.cpp
  persist/     durable state        src/persist/
    store.hpp    wal, checkpoint     store.cpp, file_lock.cpp
  net/         transport            src/net/
    frame.hpp    framing             frame.cpp
    socket.hpp   loopback TCP        socket.cpp
    protocol.hpp messages            protocol.cpp
  runtime/     process behaviour    src/runtime/
    daemon.hpp   state machine       daemon.cpp
    server.hpp   TCP front end       server.cpp
    client.hpp   blocking client     client.cpp
```

The dependency direction is strictly downward: `runtime` depends on `authority`,
`persist`, `net` and `compose`; `compose` depends on `model`; `model` depends on
`core`. Nothing in `core`, `model` or `compose` performs input or output, which
is what makes the composer a pure function.

## Core primitives

**Strong identities** (`core/id.hpp`). `StrongId<Tag>` is a 64 bit value with a
phantom tag, so a `DeviceId` cannot be passed where a `PortId` is expected. The
zero value means "unset". Comparison and ordering are by numeric value, which is
what makes canonical ordering of identity-keyed containers well defined.

**Typed outcomes** (`core/status.hpp`). `StatusCode` is fine grained;
`StatusClass` groups it into the vocabulary the invariants are stated in:
`OK`, `INVALID`, `REFUSED`, `STALE`, `CONFLICTING`, `INCOMPLETE`,
`INDETERMINATE`, `UNKNOWN`, `UNSUPPORTED`, `CANCELLED`, `ENVIRONMENT`,
`INTERNAL`. Nothing collapses `UNKNOWN` into success or into a refusal.
`Result<T>` is a two-alternative variant; domain errors never throw.

**Checked arithmetic** (`core/checked.hpp`). Every capacity computation goes
through `checked_add_u64`, `checked_sub_u64`, `checked_mul_u64` and the checked
narrowing helpers. An overflow becomes `kOutOfRange`, never a wrapped value.

**Canonical codec** (`core/bytes.hpp`). `ByteWriter` appends fixed-width
little-endian integers, length-prefixed bounded blobs and validated UTF-8 text,
and latches a failure once its configured limit is exceeded.
`ByteReader` validates every length prefix against both the caller's bound and
the bytes actually available *before* anything is allocated or read.

**Hashing** (`core/hash.cpp`). BLAKE2s-256 (RFC 7693) for content addressing and
CRC-32C (Castagnoli) for record integrity, both implemented here with no
third-party code. The test suite pins them to the published vectors.

## Model

**Evidence** (`model/evidence.hpp`). Seven record kinds - member, device, port,
link, attachment, maintenance, obligation - each carrying a `Provenance`
(source identity, source kind, that source's sequence number, observation time),
a `TopologyGeneration` and a `Durability` (`sticky` or `ephemeral`).

**The ledger** (`EvidenceLedger`) is keyed by `EvidenceSlot` =
(kind, subject, source). For each slot it stores the record with the highest
(generation, sequence). Identical content at the same rank is a duplicate;
differing content at the same rank is a conflict, resolved by the
lexicographically smaller content digest, with an order-independent
`SlotHistory` recording counts and minimum digests. Both rules together make
the ledger's canonical digest a function of the record *set* only.

## Composition

`compose(ledger, config, input)` in `compose/composer.cpp` is a pure function.
It runs in nine steps:

1. **Applicability.** Each record is filtered against the requested generation.
   A record naming a later generation is not applied to an earlier one
   (`kStaleEvidence`). An ephemeral record applies only at its own generation.
   Sticky records carry forward.
2. **Selection.** For each subject the newest applicable observation wins, with
   the same deterministic tie-break the ledger uses. A tie is marked
   `conflicted`.
3. **Membership.** A membership claim for a different rack is reported as
   `out_of_rack` and dropped. Two sources disagreeing about a device's
   incarnation make that device `indeterminate`, and it is excluded rather than
   guessed at.
4. **Devices, ports, links, attachments.** A device observation with no
   membership claim is reported as `unknown_member` and never becomes
   authoritative. A port whose owning device is not a member is `out_of_rack`.
   A port is usable only when its owning device is. A link can never carry more
   than its weakest endpoint port, and its availability is the least usable of
   the three observations. An attachment inherits the capacity of the port it
   binds to.
5. **Maintenance.** One ledger slot exists per (target, source), and the ledger
   keeps the newest record for a slot, so any source whose newest directive is a
   drain or a maintenance window keeps the exclusion alive. A replayed older
   clear cannot lift a newer drain; a clear from one source cannot lift a drain
   from another. A rack-wide directive excludes everything.
6. **Obligations.** Imported reservations naming a resource outside the rack are
   reported as `out_of_rack` and excluded. The rest are summed per target with
   checked arithmetic.
7. **Capacity.** `total` is the sum of member device capacities that are known;
   `unavailable` is the part of that belonging to devices that are ineligible,
   in maintenance or not up; `usable` is the difference. `obligated` and
   `headroom_floor` are subtracted from `usable`; whatever is left is
   `uncommitted`, and anything that does not fit is reported as `deficit`.
   Both ledger identities are re-checked before the snapshot is published.
8. **Paths.** A bounded depth-first search over eligible links enumerates short
   simple chains from an eligible access port to an uplink, fabric or peer port,
   in a deterministic order, with hop and count limits. Truncation is reported,
   never hidden.
9. **Publish.** Diagnostics are sorted and de-duplicated, the member set digest
   is computed, and the canonical encoding is hashed into the snapshot's content
   address.

**Scope expansion** (`compose/scope.cpp`) turns a root resource into the exact
resource set a lease covers: rack to everything, device to itself plus its ports,
the links that terminate on it and its attachments, port to itself plus its links
and attachments, link to itself plus both endpoints, path to itself plus every
link it traverses, attachment to itself.

## Authority

`GrantRegistry` (`authority/registry.cpp`) is the only component that decides
whether authority may exist. It keeps an occupancy index
(resource -> live grants) and a per-resource committed capacity map, both
maintained incrementally and both released exactly on fence, expiry, release or
failure.

`acquire` is idempotent on `GrantRequest::request`: the remembered outcome of a
request is returned verbatim, including refusals, which is what makes a lost
acknowledgement safe to retry.

`validate` answers in a fixed order so the refusal is always the most
informative one: token match, then durable state (released, expired, fenced,
recovering, suspended), then the current incarnation, then expiry, epoch,
generation, lifecycle, basis digest and finally per-resource eligibility and
maintenance.

**The basis digest** is the fingerprint that makes member reincarnation fence
authority. It covers, for every resource in the scope, the resource identity,
whether its capacity is known, its capacity and the (device, incarnation) pairs
it depends on, plus the rack epoch. Change any of those and every lease that
depended on them stops validating.

## Persistence

See `docs/operations.md` for the on-disk format and the recovery rules.

## Runtime

`DaemonCore` (`runtime/daemon.cpp`) owns the ledger, the composer configuration,
the grant registry, the store and the lifecycle. Every public method takes one
mutex; none of them invokes a caller supplied callback, so no lock is ever held
across foreign code. Mutations are appended to the log *before* the in-memory
state is changed, so a failed append cannot leave memory ahead of durable state.

`DaemonServer` (`runtime/server.cpp`) is a thin front end: one worker thread per
connection, each command a synchronous call into the core. It never holds the
daemon mutex while waiting on a socket.

`Client` (`runtime/client.cpp`) is a blocking one-request-at-a-time client that
verifies the response type and correlation identifier before decoding.
