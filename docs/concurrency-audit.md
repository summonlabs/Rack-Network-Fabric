# Concurrency, ownership and lifetime audit

This is the explicit audit the runtime design demands. Each hazard class is
listed with what the code does about it, what was found during the audit, and
which test covers it. Everything below was checked by reading the code, not by
assuming a pattern holds.

## Lock inventory

The runtime has exactly three mutexes.

| Mutex | Owns | Held across |
| --- | --- | --- |
| `DaemonCore::mutex_` | ledger, registry, snapshot, store, lifecycle, counters | the body of one command |
| `DaemonServer::workers_mutex_` | the worker thread list | one `push_back` |
| `Logger::sink_mutex_` | the installed log sink | a copy of the sink |

There is no shared mutex, no recursive mutex, and no condition variable.

## Self-deadlock and mutex re-entry

`DaemonCore` has one rule: a public method takes `mutex_` and then calls only
`*_locked` helpers, which never take it again.

**Defect found and fixed.** `submit_evidence` originally called the public
`expire_due`, which takes the mutex, from inside a method that already held it.
That is a guaranteed self-deadlock on the first evidence submission with an
expired lease present. It was replaced with `expire_due_locked`, and the public
wrapper now only takes the lock and forwards. `concurrency.grant_lifecycle_under_contention`
and `adversarial.concurrent_mutation_keeps_the_invariants` exercise the path.

Every other `*_locked` helper (`compose_locked`, `append_locked`,
`ensure_snapshot_locked`, `maybe_checkpoint_locked`, `transition_lifecycle_locked`)
is private and takes no lock. Grep for `lock_guard` in `src/runtime/daemon.cpp`
to confirm that no method takes the mutex twice.

## Lock ordering and inversion

The only nesting that exists is `DaemonServer::run` taking `workers_mutex_`
around a `push_back`, and that happens while nothing else is held. No code path
holds `DaemonCore::mutex_` and then takes `workers_mutex_`, or the reverse.
`Logger` is independent of both.

A lock-order cycle would require two locks to be taken in opposite orders
somewhere. With three mutexes, none of which is ever held while another is
acquired, there is no cycle to invert.

## Read to write upgrade

There is no `std::shared_mutex` in the codebase, so an upgrade deadlock cannot
occur. Readers of published state (see below) do not take any lock at all.

## Callbacks under locks

No public API accepts a callback, and no lock is held while calling out of the
class. Two places were checked specifically:

* `Logger::log` copies the sink under `sink_mutex_` and invokes it *after*
  releasing the lock, so a sink that itself logs cannot deadlock.
* `DaemonServer` never calls `DaemonCore` while holding `workers_mutex_`.

## Joining workers while holding state they need

**Defect found and fixed (two parts).** The server originally joined its workers
during shutdown while a worker could still be blocked in a blocking socket read.
Two independent problems were behind it:

1. `TcpListener::accept` polled only the server's own stop flag. When the caller
   signalled an external stop while `accept` was already polling, and nothing
   ever connected, the loop never re-checked the external flag and never
   returned. It now takes a predicate that combines both conditions and is
   polled while waiting. `concurrency.tcp_clients_are_served_concurrently` went
   from hanging indefinitely to 1.1 seconds.
2. A worker blocked in `recv_exact` on an idle connection only returned after
   the socket idle timeout, so shutdown waited for it. `recv_exact_interruptible`
   now polls the stop flag every 50 ms and returns `kCancelled` promptly. The
   idle policy is still enforced, but as a policy rather than as the mechanism
   that makes shutdown work.

Workers are joined without `mutex_` held, so a worker that is still inside a
command can finish. `concurrency.shutdown_with_a_client_mid_session` covers the
case where the client that asked for the shutdown is still connected.

## Shutdown races and stale completion

* `DaemonServer::stop_` is a single atomic; `request_stop` only stores it.
* Each worker owns a `shared_ptr<atomic<bool>>` "finished" flag. `reap_workers`
  joins a worker only when its flag is set, or unconditionally at shutdown. No
  worker writes to the worker list; only the accept thread does.
* `DaemonCore::stop` is idempotent, and `adversarial.stop_is_idempotent_and_compose_after_retire_is_refused`
  calls it twice.
* Repeated start and stop cycles are covered by
  `adversarial.repeated_start_and_stop_is_stable` and
  `adversarial.server_start_stop_cycles_release_every_resource`.

## Iterator and reference invalidation

* `GrantRegistry::detach` mutates `occupancy_` and `committed_` while holding a
  `const Grant&` that points into `grants_`. It never erases from `grants_`, so
  the reference stays valid for the whole call, including when the caller is
  iterating `grants_` (`fence_all`, `fence_device`, `expire_due`,
  `suspend_all`, `resume_all`, `revalidate`). Those loops mutate values in place
  and never insert or erase, so no iterator is invalidated.
* `detach` erases from `occupancy_` while iterating `footprints_.at(id)`. Those
  are different containers, so erasing one does not invalidate the other.
* `Server::reap_workers` builds a new vector and move-assigns it. `std::thread`
  is movable, and the joined threads are joined before the assignment.
* The composer builds `data.devices` and only then takes pointers into it
  (`device_record`); no pointer is held across a push_back.

## Use after free and lifetime

* `Snapshot` is a `shared_ptr` to an immutable `SnapshotData`. Once published,
  the data is never mutated, so any number of threads may read one while the
  composer builds the next generation.
  `concurrency.snapshot_handles_are_safe_to_read_concurrently` runs six readers
  against a producing thread and re-derives the digest on every read.
* `DaemonCore` outlives `DaemonServer` in every use site, including the test rig
  and `rnfd` itself. `DaemonServer` holds a reference, not an owner.
* `Snapshot::data()` returns a reference to a function-local static empty
  snapshot when the handle is empty. Function-local statics have thread-safe
  initialisation, so that fallback is safe to reach from any thread.

## Resource leaks

* `TcpStream` and `TcpListener` close in their destructors and release before
  adopting a new handle in move assignment.
* `FileLock` releases the platform handle in its destructor; the operating
  system also releases it when the process dies, which is what makes a hard kill
  safe to restart from.
* `Store` closes the log file in its destructor.
* `GrantRegistry` releases exactly the pools a grant took, stored in
  `Grant::capacity_pool`, so a release cannot over- or under-return even after
  the topology moved on.
* `ChildProcess` kills and reaps its child in the destructor.
* `adversarial.server_start_stop_cycles_release_every_resource` and
  `concurrency.store_open_close_cycles_preserve_every_record` cover repeated
  open and close cycles.

## Ordering and durability

* Mutations append to the log *before* the in-memory state changes, so a failed
  append cannot leave memory ahead of durable state. If the second of the two
  appends in `acquire` fails, the grant is released so memory and log agree.
* `maybe_checkpoint_locked` runs after a command completes rather than in the
  middle of one, so a checkpoint never captures a half-applied command.
* `grant_to_response` is `const`; the response is built from a copy of the
  grant, so the response cannot observe a later mutation.

## Bounded blocking

Every blocking wait is bounded by a policy that is documented, not by a test
deadline: the accept loop polls every 50 ms, connection reads poll every 50 ms
and apply an idle policy, and the lease expiry ticker runs every 200 ms. No test
in this repository sets a CTest `TIMEOUT`, uses a watchdog, or treats a forced
termination as a pass.
