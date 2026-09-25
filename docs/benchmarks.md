# Benchmarks

These figures are measured, not modelled. They come from
`build/bench/rnf_bench.exe 7` on the machine described below, with the source
revision that produced this document. Every row prints its operation count, so a
reader can reproduce or contradict any number here.

## How to reproduce

```console
cmake --preset bench
cmake --build --preset bench
build/bench/rnf_bench.exe 7
```

The argument is the number of repetitions; the reported value is the median of
those repetitions. There is no warm-up fudge, no best-of, and nothing is
extrapolated from a smaller or larger input.

## Machine

| | |
| --- | --- |
| CPU | AMD Ryzen 7 9800X3D, 8 cores / 16 threads, 4.7 GHz |
| Operating system | Microsoft Windows 11 Pro, version 10.0.26200 |
| Toolchain | MSVC 19.44.35222 x64, Release (`/O2`) |
| Build | CMake 4.3.2, Ninja, `-DCMAKE_BUILD_TYPE=Release` |

Numbers from a different machine, compiler or build type will differ. They are
reported to show the shape of the cost, not to promise a figure.

## Results

Median of 7 repetitions.

| Benchmark | Median ms | Operations | Per second |
| --- | ---: | ---: | ---: |
| blake2s-256 over 64 KiB | 0.108 | 65536 bytes | 608,505,107 B/s |
| crc32c over 64 KiB | 0.100 | 65536 bytes | 657,331,996 B/s |
| evidence record decode | 23.237 | 200000 records | 8,607,074 /s |
| evidence record digest | 48.109 | 200000 records | 4,157,261 /s |
| compose 2x4 (36 records) | 0.100 | 1 snapshot | 10,030 /s |
| compose 4x16 (232 records) | 0.776 | 1 snapshot | 1,289 /s |
| compose 8x32 (848 records) | 3.356 | 1 snapshot | 298 /s |
| ledger insert | 0.157 | 232 records | 1,476,766 /s |
| grant acquire + release | 142.090 | 20000 grants | 140,755 /s |
| durable append (fsync per record) | 420.411 | 500 records | 1,189 /s |
| buffered append (no fsync) | 46.548 | 20000 records | 429,663 /s |
| daemon round trip: query_state | 62.302 | 2000 requests | 32,102 /s |
| daemon round trip: acquire + release | 143.161 | 2000 requests | 13,970 /s |

## What the numbers mean

**Composition is linear in the evidence set.** A 2x4 leaf/spine rack (36
records, 6 devices, 8 links) composes in 0.10 ms. A 4x16 rack (232 records, 20
devices, 64 links) takes 0.78 ms. An 8x32 rack (848 records, 40 devices, 256
links) takes 3.36 ms. The growth tracks the record count, which is what the
design intends: composition is a sort-and-join over an already keyed map, plus a
bounded path search.

**Durable appends cost what a flush costs.** 1189 records per second with
`fsync` per record, against 429,663 per second buffered, is a factor of about
360 and is entirely the platform flush. This is the honest price of the
guarantee that an acknowledged evidence record survives a hard kill; the
multiprocess tests depend on it.

**A grant costs about 7 microseconds in process.** `grant acquire + release`
does three scope expansions (acquire, attach, detach), two digests (request
identity and authority basis), and updates the occupancy and capacity indexes.
At 140,755 per second it is not the bottleneck of any realistic rack.

**A loopback round trip costs about 31 microseconds.** `query_state` moves a
full state response over a real TCP socket in 62.3 ms per 2000 requests. The
`acquire + release` round trip is 143.2 ms per 2000, or about 72 microseconds
per request pair, which is consistent with the in-process cost of the two
commands plus two round trips.

**Hashing is not a constraint.** BLAKE2s-256 runs at 608 MB/s and CRC-32C at
657 MB/s on this machine, both single threaded and both implemented here with no
intrinsics.

## What is deliberately not benchmarked

* No network fabric, switch, ASIC, RDMA device or vendor SDK was measured,
  because none was used. Any figure for those would be fabricated.
* No multi-host numbers, because the transport binds loopback only.
* No end-to-end latency under load or at scale, because no such run was made.
