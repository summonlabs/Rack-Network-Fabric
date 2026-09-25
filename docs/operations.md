# Operations

## Running a daemon

```console
rnfd --rack 7 --name rack-7 --store ./rack7 --port 8790 \
     --headroom 100 --checkpoint-every 4096 --auto-compose --log-level info
```

| Flag | Meaning |
| --- | --- |
| `--rack` | rack identity this daemon is authoritative for (required) |
| `--name` | human readable name |
| `--store` | durable state directory; omit for a memory-only daemon |
| `--port` | loopback port; 0 asks the operating system for a free one |
| `--host` | loopback address; anything other than 127.0.0.1 or localhost is refused |
| `--headroom` | capacity held back from every grant |
| `--max-connections` | concurrent connection bound |
| `--max-frame` | frame payload bound |
| `--checkpoint-every` | log records between automatic checkpoints; 0 disables |
| `--default-ttl-ms` | lease lifetime applied when a request asks for 0 |
| `--auto-compose` | recompose after every accepted evidence batch |
| `--no-fsync` | keep the log in the page cache only; benchmarks only |
| `--log-level` | trace, debug, info, warn, error, off |

The daemon prints exactly one readiness line before serving:

```
RNFD READY port=8790 rack=0000000000000007 incarnation=1 generation=0 epoch=0 lifecycle=assembling pid=12345
```

On a clean stop it prints `RNFD STOPPED`. `SIGINT` and `SIGTERM` request the same
orderly shutdown as the `shutdown` command.

## On-disk state

```
rnf.lock        exclusive lock for the lifetime of the process
rnf.meta        256 byte manifest
rnf.checkpoint  accepted evidence plus the composed snapshot at an offset
rnf.wal         append-only records
```

The manifest holds the format version, the rack binding, the incarnation
counter, the checkpoint offset and length, the snapshot and ledger digests, and
the clean-shutdown flag. It is covered by a CRC-32C and written by
rename-into-place, so it is either the old manifest or the new one.

Each log record is `[u32 length][u32 CRC-32C][payload]`. The payload starts with
a one byte record type: rack binding, configuration, evidence, lifecycle,
generation, epoch, grant commit, grant state, request outcome, clean shutdown.

### Recovery rules

1. A manifest written by a different format version is refused. The store is
   never reinterpreted.
2. A manifest that fails its checksum is refused by default, because it holds
   the only durable copy of the incarnation counter. `StoreOptions::allow_manifest_loss`
   opts into replaying the log from the start and deriving the counter from the
   highest incarnation the log mentions; the recovery report says
   `manifest_rejected`.
3. If the checkpoint is missing or fails integrity, replay starts at offset 0.
   The recovery report says `checkpoint_rejected`; nothing is lost because the
   log holds every record.
4. Replay stops at the first record that is incomplete or fails its checksum.
   A partial record at the end of the log is reported as `truncated_tail`; a bad
   record followed by more data is reported as `corrupt_record`. In both cases
   the unverifiable tail is truncated away so a later append never follows a
   partially written record.
5. Recovered grants are installed in `recovering`: they reserve their capacity
   but report `INDETERMINATE` until revalidated. Revalidation happens
   automatically at start-up under the new incarnation; a grant whose scope,
   epoch or basis changed is fenced instead of being silently re-armed.
6. An unclean shutdown puts the rack into `recovering`. New authority is refused
   until an operator moves it forward, which is deliberate: the process that died
   may have left work in flight.

Every record application is idempotent, so replaying a prefix twice is safe.

## Lifecycle

```
assembling --> active <--> degraded
     |            |  \
     |            v   v
     |         draining --> maintenance
     |            |            |
     +---------> recovering <--+
                  |
                  v
               retired (terminal)
```

* `assembling`: accepting evidence, refusing new authority.
* `active`: normal operation.
* `degraded`: normal operation; the snapshot reports why resources are unusable.
* `draining`: no new authority; existing leases stay valid and usable.
* `maintenance`: no new authority; existing leases are suspended and cannot be
  used, but they keep their capacity until they expire or are released.
* `recovering`: entered after an unclean restart; no new authority until an
  operator moves the rack forward.
* `retired`: terminal; no further transition is accepted.

Illegal transitions are refused with `invalid_lifecycle_transition` and the
resulting state is reported, so a client always sees where the rack really is.

## Inspecting a rack

```console
rnfctl --endpoint 127.0.0.1:8790 state       # digests, capacity ledger, lifecycle
rnfctl --endpoint 127.0.0.1:8790 resources   # per resource eligibility and capacity
rnfctl --endpoint 127.0.0.1:8790 paths       # enumerated paths and bottlenecks
rnfctl --endpoint 127.0.0.1:8790 grants      # live grants with their tokens
rnfctl --endpoint 127.0.0.1:8790 stats       # counters
```

The capacity fields in `state` are:

| Field | Meaning |
| --- | --- |
| `total_capacity` | sum of member device capacities that are known |
| `unavailable_capacity` | part of the total belonging to unusable devices |
| `usable_capacity` | `total - unavailable` |
| `obligated_capacity` | imported reservations |
| `committed_capacity` | capacity currently held by live grants |
| `headroom_floor` | configured reserve |
| `uncommitted_capacity` | `usable - obligated - headroom`, never below zero |
| `capacity_deficit` | how much obligations plus headroom exceed `usable` |

`usable + deficit == obligated + headroom + uncommitted` always holds.

## Maintenance and drain

```console
rnfctl --endpoint 127.0.0.1:8790 maintenance --action drain --target device:2 --reason firmware
rnfctl --endpoint 127.0.0.1:8790 maintenance --action clear --target device:2 --reason done
```

Directives are per source and monotone: the newest directive from a source wins
for that source, and a target is excluded while *any* source's newest directive
is a drain or a maintenance window. Replaying an older message changes nothing,
and a clear from one source cannot lift a drain issued by another.

## Backing up and restoring

The store directory is self-contained. To back it up, stop the daemon and copy
the directory. Copying a live directory is not supported: the manifest and the
log are written independently.

## Failure checklist

| Symptom | Meaning |
| --- | --- |
| `busy` at start-up | another process holds `rnf.lock` |
| `integrity_failure` at start-up | the manifest failed its checksum |
| `incompatible_version` at start-up | the store was written by another format version |
| `out_of_rack` at start-up | the store belongs to a different rack |
| `lifecycle_refused` on acquire | the rack is not in `active` or `degraded` |
| `fenced_incarnation` on validate | the lease belongs to a previous incarnation, or its scope dependencies changed |
| `stale_epoch` on validate | the member set changed |
| `indeterminate` on validate | the lease was recovered and not yet revalidated |
| `scope_not_eligible` on acquire | a resource in the scope is ineligible or in maintenance |
