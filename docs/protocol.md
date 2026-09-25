# Protocol

The daemon speaks one framed request/response protocol over TCP. It binds the
loopback address only; binding anything else is refused with `unsupported`.

## Framing

Every message is one frame, 24 byte header followed by the payload.

| Offset | Size | Field | Notes |
| --- | --- | --- | --- |
| 0 | 4 | magic | `0x31464E52`, that is "RNF1" in little-endian |
| 4 | 2 | version | protocol version, currently 1 |
| 6 | 2 | message type | see the table below |
| 8 | 4 | payload length | validated before allocation |
| 12 | 4 | payload CRC-32C | recomputed and compared by the receiver |
| 16 | 8 | correlation | echoed in the response |

Bounds:

* the negotiated payload ceiling defaults to 1 MiB (`--max-frame`);
* the hard ceiling is 8 MiB and applies even when a larger value is negotiated;
* a frame declaring a length above either ceiling is refused with
  `oversize_field` before a buffer is allocated;
* a length prefix that exceeds the bytes actually received is refused with
  `truncated`;
* an unknown message type is refused with `unsupported`;
* a version mismatch is refused with `incompatible_version`.

A framing failure is answered with an error frame and then the connection is
dropped, because the stream cannot be resynchronised after a bad header. A
command failure is answered with an error frame and the connection stays usable.

## Message set

| Type | Value | Direction | Body |
| --- | --- | --- | --- |
| `hello` | 1 | client -> daemon | protocol version, client id |
| `hello_ack` | 2 | daemon -> client | version, accepted, rack, epoch, generation, incarnation, lifecycle, snapshot digest |
| `submit_evidence` | 16 | client -> daemon | a bounded batch of evidence records |
| `evidence_ack` | 17 | daemon -> client | one outcome per record |
| `compose` | 32 | client -> daemon | target generation |
| `compose_ack` | 33 | daemon -> client | code, generation, epoch, incarnation, snapshot digest, diagnostic count |
| `acquire` | 48 | client -> daemon | request id, principal, scope, mode, capacity, ttl |
| `acquire_ack` | 49 | daemon -> client | code plus the grant summary |
| `renew` | 50 | client -> daemon | lease token, ttl |
| `renew_ack` | 51 | daemon -> client | code plus the grant summary |
| `release` | 52 | client -> daemon | lease token |
| `release_ack` | 53 | daemon -> client | code |
| `validate` | 54 | client -> daemon | lease token |
| `validate_ack` | 55 | daemon -> client | code plus the grant summary |
| `lookup` | 56 | client -> daemon | request id |
| `lookup_ack` | 57 | daemon -> client | remembered flag, stored outcome, grant id, fence, incarnation |
| `query_state` | 64 | client -> daemon | empty |
| `state_ack` | 65 | daemon -> client | full rack summary plus bounded diagnostics |
| `query_resources` | 66 | client -> daemon | limit, optional kind filter |
| `resources_ack` | 67 | daemon -> client | resource list with eligibility, capacity and availability |
| `query_paths` | 68 | client -> daemon | empty |
| `paths_ack` | 69 | daemon -> client | path list with bottleneck and eligibility |
| `query_grants` | 70 | client -> daemon | empty |
| `grants_ack` | 71 | daemon -> client | grant summaries including tokens |
| `lifecycle` | 80 | client -> daemon | target state |
| `lifecycle_ack` | 81 | daemon -> client | code, resulting state |
| `checkpoint` | 82 | client -> daemon | empty |
| `checkpoint_ack` | 83 | daemon -> client | code, log offset |
| `stats` | 84 | client -> daemon | empty |
| `stats_ack` | 85 | daemon -> client | counters |
| `shutdown` | 86 | client -> daemon | empty |
| `shutdown_ack` | 87 | daemon -> client | code |
| `error` | 32512 | daemon -> client | status code plus detail text |

## Body encoding

Bodies use the same canonical codec as the snapshot: fixed-width little-endian
integers, `u32` length prefixes for text and blobs, no padding. Text fields are
validated as UTF-8 with no embedded NUL and are bounded (256 bytes for a host
key, 256 for a detail string, 512 for a diagnostic).

Decoding bounds, applied before any allocation:

| Field | Bound |
| --- | --- |
| evidence records per batch | 4096 |
| resources per response | 65536 |
| paths per response | 8192 |
| grants per response | 8192 |
| diagnostics per response | 512 |

The daemon additionally applies its own `--max-connections` bound (default 32)
and refuses a connection beyond it by accepting and immediately closing it,
counting it in `connections_rejected`.

## Lease tokens

A token is `(grant id, fencing token, controller incarnation, basis digest)`.
The client receives it from `acquire_ack`, `renew_ack`, `validate_ack` or
`grants_ack`. A token is only meaningful together with the incarnation that
issued it: presenting a token from a previous incarnation returns
`fenced_incarnation` even when every other field matches.

## Resolving a lost acknowledgement

If a client is killed or its connection drops between sending `acquire` and
receiving `acquire_ack`, it does not know whether the request was committed.
The resolution path is:

1. `lookup` with the original request id.
2. If the answer is `UNKNOWN`, this daemon has no durable record of the request.
   That answer is honest: a request that was refused before it was durably
   remembered and a request that never arrived are indistinguishable, and neither
   created a grant.
3. If the answer is remembered and `ok`, the grant exists under the returned
   grant id. Query `grants` to read its current token, which is issued by the
   incarnation that is running now.

Replaying the original `acquire` with the same request id is always safe: it
returns the remembered outcome and never creates a second grant.
