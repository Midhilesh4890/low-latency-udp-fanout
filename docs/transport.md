# Transport operations

## Delivery guarantees

There are three explicit modes:

| Mode | Acceptance means | Failure behavior |
|---|---|---|
| UDP, requiring --allow-insecure-udp | Accepted by the local kernel | Loss is possible; optional FEC repairs within its budget |
| Unix streams plus ordinary TLS relay | Written into a reliable transport stream | Link/process failure is reported; no persistent replay |
| Unix streams plus durable TLS relays | With --durable-acks, committed to the sender relay's SQLite outbox | Retained until the receiving relay commits and acknowledges; pending entries survive restart |

Durable mode is **at least once**. The receiving relay commits each unique
session/sequence pair before its network ACK. It retains the inbox entry until
the C++ receiver acknowledges publication. Lost network ACKs do not enqueue a
second inbox copy. A crash after ring publication but before local ACK/retirement
can replay that frame. Receiver deduplication suppresses duplicates while that
receiver remains alive; application side effects across receiver/consumer
restarts still need transactional idempotency using stream/epoch/sequence.

A transport ACK is not proof that a consumer has committed a business operation.
Shared-memory rings are volatile. The durable guarantee begins when the local
relay commits; frames not yet admitted to that relay are not crash-persistent.
Do not discard or recreate relay databases to reconnect. Preserve the inbox as
well as the outbox, and back up their identity metadata together.

## Secure and durable setup

Python 3, SQLite and TLS 1.3 support are required; these modules are in the
standard library on typical Linux installations. Supply CA-issued certificates
with appropriate extended key usage. The receiver certificate must identify
receiver.example. The sender certificate must have exact DNS SAN sender.example.
Both directions verify the CA; the receiving relay additionally pins that
exact sender SAN. Store private keys and databases in private service-owned
directories. All paths and identities below are examples.

Start these on the receiving host, in separate terminals:

~~~bash
./build/bin/receiver --unix-listen /run/user/1000/pulse-rx.sock \
  --out-shm /pulse_out --slots 8192 --count 5000 --idle-ms 60000 \
  --ack-publish --stream-reconnect

./build/bin/consumer --shm /pulse_out --slots 8192 \
  --count 5000 --idle-ms 60000

python3 scripts/tls_relay.py receive --host 0.0.0.0 --port 9443 \
  --unix-path /run/user/1000/pulse-rx.sock \
  --ca ca.pem --cert receiver.pem --key receiver.key \
  --peer-name sender.example --durable-db inbox.db
~~~

On the sender host, start the relay and wait for READY:

~~~bash
python3 scripts/tls_relay.py send --host receiver.example --port 9443 \
  --unix-path /run/user/1000/pulse-tx.sock \
  --ca ca.pem --cert sender.pem --key sender.key \
  --peer-name receiver.example --durable-db outbox.db
~~~

Then start producer and sender in separate terminals:

~~~bash
./build/bin/producer --shm /pulse_in --slots 8192 \
  --count 5000 --rate 1000 --wait-readers 1

./build/bin/sender --in-shm /pulse_in --slots 8192 \
  --count 5000 --unix-dst /run/user/1000/pulse-tx.sock --durable-acks
~~~

The durable receive relay is a service and remains available for reconnects.
After a sender relay crash, drain accepted frames without new ingress by
restarting the same send command with --replay-only. Its database preserves the
original relay session and monotonically increasing sequence. --retry-seconds
(default 30) bounds a stalled reconnect/drain period; expiration exits with
failure while leaving undelivered records on disk. Replaying after expiration
uses the same command and database.

--spool-bytes bounds pending payload bytes (default 64 MiB, supported 1 MiB?1 GiB).
SQLite page/WAL limits bound additional database growth. Full queues exert
bounded backpressure, then fail; they never silently evict accepted entries.
The spool's configured peer identity is persisted and checked on reopen.
A file lock prevents two relay processes from sharing one database concurrently.
Databases are specific to send/receive mode and expected peer identity.

Omit --durable-db and both acknowledgement options for ordinary TLS streaming.
The two relay peers must use the same mode. Raw local UDP relay endpoints remain
an explicit best-effort alternative; durable mode requires Unix streams.

## Shared memory and overload

Layout 3 uses robust process-shared mutexes for slot copies and registration,
eliminating simultaneous non-atomic payload reads/writes. A writer invalidates
a slot before copying; recovery after a dead lock owner cannot expose a partial
message. Robust mutex recovery prevents a dead lock owner from wedging a slot.

Up to 64 local readers register cursors. By default, a producer waits before
overwriting unread slots. Reader cursors advance after a frame is safely copied;
this is a memory reuse acknowledgement, not a durable application commit.
A slow, absent or dead reader causes explicit failure after a two-second wait.
A dead reader is not silently removed, because that would hide possible loss.
Restart the supervised group with new rings. Readers attaching after history
has already been overwritten cannot recover that old history.

PULSEFANOUT_RING_POLICY=overwrite intentionally permits lapping for experiments;
copies still remain synchronized and a lapped reader gets an explicit status.
Only one creator/writer is supported per ring. Creation is exclusive; initialization
is locked; size and layout metadata are checked. Normal failures clean up owned
segments. Supervisors use fresh epoch-scoped names so orphaned prior instances
are never silently reset.

## Independent destinations

Each destination has one worker and a separate queue, bounded by --queue-bytes
(default 4 MiB). The active batch is additional bounded memory. The total
configured queued-byte budget cannot exceed 1 GiB. --send-workers is a thread
budget (default 256) and must cover every configured destination; no shared
worker can be monopolized by another destination.

A queue overflow or send error isolates that destination and records rejected
frames. Healthy destinations continue. Completion drains healthy queues, prints
per-destination results, and returns nonzero if any destination failed.
Frames already sent cannot be rolled back. Use independently durable relays for
destinations that must retain admitted messages across failures. Finite capacity
cannot provide unlimited lossless buffering for an indefinitely unavailable peer.

## Wire compatibility and stream identity

Every network datagram/stream frame starts with PFS3, followed by big-endian
stream_id:u64 and stream_epoch:u64. Its body is either an FEC2 shard or a PUL3
application frame. The sender assigns random identities unless --stream-id and
--stream-epoch are supplied. Preserve both only when deliberately continuing a
stream with non-reused application sequence numbers. A new producer incarnation
should use a new epoch.

PUL3 contains explicit fields in message.h declaration order, with no compiler
padding. Integers are fixed-width big endian and signed values use two's
complement. Doubles use IEEE754 binary64; arrays have fixed declared lengths.
Application schema version is 2. Header body_len includes the PUL3 magic but
excludes outer multiplexing/FEC headers. Stream/epoch fields inside the application
header must match the outer envelope.

Trade, BBO and order-book application frames are 200, 196 and 540 wire bytes.
Unknown magic/type/version, inconsistent identities, truncation, and trailing
bytes are rejected. Native layout is confined to each local shared-memory ring.
All old network peers and old shared-memory mappings must be upgraded together.

Receiver --max-streams defaults to 16 and is bounded to 256. Every identity has
separate FEC/deduplication state. New identities beyond the limit are rejected
rather than evicting state and reopening an old replay window. A configured
worst-case stream/FEC memory estimate above 512 MiB is rejected. Unauthenticated
UDP identities are labels, not proof of identity; secure transport authenticates
the publisher. The consumer accounts for independent sequence ranges per stream.

FEC2's 17-byte header is explicitly encoded and no longer uses a packed C++
object representation. It contains magic, generation:u32, index:u16, k:u16,
parity_count:u16, protected_len:u16, close_reason:u8. All are big endian.
Protected shard lengths are explicitly little endian, followed by application
bytes and zero padding.

## FEC and telemetry bounds

--fec-k 8 --fec-parity 3 reconstructs eight data shards from any eight of eleven
surviving shards. The systematic Cauchy code uses GF(256), polynomial 0x11d;
its first parity row is XOR. k is capped at 128, parity at 16. Timeout closures
announce their actual size. Generation storage is bounded and expires after one
second. Late recovered data may be published out of order. More losses than the
parity budget require reliable retransmission, not a larger claim about FEC.

Every telemetry distribution retains at most 65,536 deterministic reservoir
samples. Consumer totals, extrema and means remain exact; sampled quantiles and
FEC recovery reports are approximate after the cap. Consumer --csv records are
written incrementally as native binary records of five uint64 fields: sequence,
send timestamp, receive timestamp, stream ID, epoch. This instrumentation export
is not the durable delivery log. Test-impairment pending queues are also bounded.

## Service supervision and discovery

scripts/run_pipeline.py is the local finite UDP benchmark supervisor.
scripts/supervise_services.py takes a JSON configuration with services, optional
completion service, and advertised endpoints. Each service has a name, argv
array, and optional ready output substring. No shell is used. The manager
restarts the group on failures, drains logs, terminates children on shutdown,
and atomically publishes --state-file JSON.

Command arguments support {runtime} (a new private directory per restart) and
{epoch} (a new identifier). Keep durable databases outside {runtime}. Use
/pulse_{epoch} for fresh shared-memory names and {runtime}/relay.sock for sockets.
The optional completion service should be the final consumer, not the sender,
when downstream processing is the intended completion boundary.

Example service configuration:

~~~json
{
  "services": [
    {
      "name": "relay",
      "argv": [
        "python3", "scripts/tls_relay.py", "receive",
        "--host", "0.0.0.0", "--port", "9443",
        "--unix-path", "{runtime}/receiver.sock",
        "--ca", "ca.pem", "--cert", "receiver.pem", "--key", "receiver.key",
        "--peer-name", "sender.example", "--durable-db", "inbox.db"
      ],
      "ready": "LISTENING"
    },
    {
      "name": "receiver",
      "argv": [
        "build/bin/receiver", "--unix-listen", "{runtime}/receiver.sock",
        "--out-shm", "/pulse_{epoch}", "--slots", "8192",
        "--ack-publish", "--stream-reconnect", "--idle-ms", "86400000"
      ]
    },
    {
      "name": "consumer",
      "argv": [
        "build/bin/consumer", "--shm", "/pulse_{epoch}",
        "--slots", "8192", "--idle-ms", "86400000"
      ]
    }
  ],
  "endpoints": {"feed": "receiver.example:9443"}
}
~~~

~~~bash
python3 scripts/supervise_services.py services.json \
  --state-file services-state.json --restarts 3

python3 scripts/discovery.py serve --host 0.0.0.0 --port 9444 \
  --state-file services-state.json --ca ca.pem \
  --cert receiver.pem --key receiver.key --client-name sender.example

python3 scripts/discovery.py fetch \
  --url https://receiver.example:9444/v1/services \
  --ca ca.pem --cert sender.pem --key sender.key
~~~

Discovery requires mutual TLS, checks allowed client DNS identities, disables
client redirects, bounds response size, and serves 503 for absent/stale process
registrations. It is a single authenticated registry, not a replicated consensus
service. Clients can fetch endpoints and monitor epochs; network routing and
high availability still belong to deployment configuration.

## Validation scope

Unit/integration tests cover concurrent overwrites, backpressure, dead mutex
owners, bounded metrics, multi-publisher collisions, stalled destination
isolation, certificate rejection, durable sender-relay crash/replay, persistent
inbox deduplication, supervisors and authenticated discovery.

A local WSL run delivered 200,000 mixed messages at a configured 50,000/s with
zero reported drops. This is a reproducible local check, not evidence of WAN
latency, cross-architecture execution, multi-day stability or production
capacity on other hardware.
