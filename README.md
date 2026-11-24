# Tributary
### A Kafka-style replicated, partitioned commit log.

Tributary is a small distributed log. A cluster of brokers stores ordered, append-only streams of records, replicates them across machines, and keeps serving requests through broker crashes and network partitions. It follows the core design of [Apache Kafka](https://kafka.apache.org/) with partitioned logs, pull-based replication, in-sync replica sets, a high watermark, leader epochs, and consumer groups. 


## Highlights
- **Durable segmented storage.** Each partition is a directory of segment files holding Castagnoli-CRC-32-protected record batches alongside a sparse offset index. On restart, recovery truncates any torn writes and keeps everything before it.
- **Replication with in-sync replica sets.** Followers pull updates from leaders, and leaders track which followers are caught up. A high watermark notes the latest record that all followers in the in-sync replica set (ISR) possess, and advance it as data is replicated.
- **Automatic failover with fencing.** A controller detects dead brokers through heartbeats, elects a new leader from the ISR, and bumps a leader epoch to prevent stale writes from being committed.
- **Consumer groups.** Partitions are divided among group members, offsets are committed, and membership changes trigger a rebalance. Generation numbers fence out stalled consumers.
- **Fault injection without root.** A user-space layer drops, delays, or blocks traffic between chosen nodes, so network partitions can be tested on shared machines without requiring Docker or `iptables`.
- **Reproducible evaluation.** Experiments exist as independent scripts. They can start the cluster, run the workload, apply the fault schedule, check correctness, and plot results.
- **No dependencies.** Standard C++17, POSIX sockets, and GNU Make. Python is only used to analyze logs and draw plots.

## How it works

### Architecture Overview
<img width="4640" height="2040" alt="log_diagram_background" src="https://github.com/user-attachments/assets/aba9b695-c044-4374-ab99-68f2e1099e65" />

Kafka’s core abstraction is the log: an append-only sequence of records. Each record is defined by its offset, or its index in the log. Records are never modified, and reading them does not remove them. Many independent applications can therefore consume the same stream of data at its own pace, which is the primary difference between this log and a traditional message queue.

A single log on a single machine can neither scale nor survive failures. Tributary therefore divides each named stream, or topic, into several partitions. Each partition is an independent log, meaning different partitions in the same topic have different record histories. To better manage disk space and enable more efficient I/O through indexing, these partitions are divided into segment files. A producer chooses a partition by hashing the record’s key, so all records with the same key stay in one partition and maintain their relative ordering.

Partitions are replicated onto several servers called brokers. Brokers can contain multiple partitions across different topics, though it will never hold two of the same partition. In this way, a partition can be said to refer to the logical object of all logs on all brokers that are assigned that partition. The copy on one broker is itself a replica. One replica in the partition is the leader, while all others are followers. The leader accepts all reads and writes, while followers continuously pull new records from the leader. Leadership is assigned per-partition, meaning a broker can be the leader of some partitions and a follower for others, allowing the write load to be spread across the cluster.

Replication requires consideration of when a write is considered safe. The leader tracks which followers are keeping up with updates. These followers form the in-sync replica set (ISR). A record is considered ‘committed’ once every in-sync replica holds it. The partition leader tracks this boundary with a high watermark. Consumers can only ever see committed records (those before the high watermark).

<img width="2640" height="1570" alt="replica_watermark_background" src="https://github.com/user-attachments/assets/b17d3495-75d7-448b-9274-6fcddbe0699e" />

Producers write to the partition leader, and can be configured to request one of three levels of acknowledgement, which alter the stage at which the broker responds:
acks = 0: fire and forget. The Producer does not wait for any broker response before continuing. This has the lowest latency and highest throughput, but provides no durability guarantees.
acks = 1: leader acknowledgement. The leader responds to the Producer as soon as it writes the record to its own local log. This provides moderate durability and performance, though the record is lost if the leader crashes before followers can replicate it.
acks = all/acks = -1: full ISR acknowledgement. The leader waits for all replicas in the ISR to write the record before sending an acknowledgement to the Producer. This is where the main durability promise lies, and is the default behavior.

<img width="3840" height="1770" alt="system_overview_background" src="https://github.com/user-attachments/assets/ac240226-3f6d-4525-9620-f2a67ee92630" />

<img width="2800" height="1560" alt="replication_background" src="https://github.com/user-attachments/assets/13ca3f53-d6aa-41d2-8fa0-b815418333e6" />

The system must also be resilient to broker failures, and track which broker holds the leader replica for a given partition. Tributary handles this with a single Controller. The Controller never touches records, and instead tracks broker health and membership. All brokers send the Controller periodic heartbeats, and the Controller keeps a copy of each partition’s ISR. If a leader’s heartbeats stop, the Controller promotes the first live member of the partition’s ISR to be the new leader. If none are alive, the partition is marked inactive until one returns.

Partitions carry a leader epoch, a monotonic counter that is incremented every time a new leader is elected. Messages from previous epochs carry an outdated epoch number and are rejected, which prevents a leader that was slow or cut off from continuing to commit writes (fencing). This also prevents misconfiguration due to out-of-order leadership messages - (e.g. say B1 is elected at epoch 5 and immediately dies, then B3 is elected at epoch 6. If B2 receives the epoch 6 update then the epoch 5 update, it won’t erroneously think B1 is now the leader). Note that in this project, the Controller is assumed to never crash.

A similar idea governs consumer groups, which are a set of consumers that share a common identifier. Members of consumer groups split a topic’s partitions and work in parallel, with each partition having one owner per consumer group. Consumers can join a consumer group by requesting the Controller, which will balance the load accordingly, ensuring request load is spread across partitions. Consumers in a consumer group must therefore send heartbeats to the Controller, and dropped heartbeats trigger the Controller to remove the member and rebalance. Consumer groups also use fencing via a group generation counter, incremented on every rebalance. If a consumer is running slow and misses a rebalance, the outdated group generation ensures messages are not sent to the incorrect broker or stalled consumers from overwriting the new owner’s offsets after rebalancing.

<img width="2920" height="1160" alt="consumer_groups_background" src="https://github.com/user-attachments/assets/d250536e-e2f0-4f86-8b71-5f581c7f9f84" />

### Storage Design
Each replica is a directory of segment files, holding CRC-protected batches provided by Producers. A recovering broker scans the newest segment and cuts the log off at the first batch that fails validation, effectively removing incomplete batch writes.
Rough directory layout for a given replica
```bash
tributary/
├── broker-1/
│   ├── orders-0/
│   │   ├── 00000000000000000000.log   # batches, offsets 0–499
│   │   ├── 00000000000000000000.index # sparse offset → byte position
│   │   ├── 00000000000000000500.log   # active segment
│   │   ├── 00000000000000000500.index
│   │   └── state                      # leader epoch + HW checkpoint
│   └── orders-1/ …
└── controller/
    └── offsets.log                # committed consumer offsets
```
Segment files (.log) are named by the base offset zero-padded to 20 digits, meaning a lexical sort is identical to an offset sort. 

The .index files are small lookup tables that let a broker jump close to an offset in its matching .log file without needing to scan the segment from the start. This enables the use of variable-length records while still providing a quicker way to search through the log without affecting durability. These files can be rebuilt from log data whenever needed.

Each segment has exactly one index (one .index file for every .log). The file itself is a flat array of 8-byte entries: a 4-byte integer for the relative offset (record offset minus segment offset), and a 4-byte integer for the byte position of the batch in the segment file.

For example, suppose segment 1000.log has the following index entries:
| relative_offset | position |
| :---: | :---: |
| 0 | 0 |
| 31 | 4,180 |
| 66 | 8,350 |
| 98 | 12,500 | 

To read the record at offset 1080 (which would be relative offset 80), the floor entry would be (66, 8350). The broker can immediately start reading 1000.log from byte 8350, then scan through batch headers until it reaches the batch containing record 1080. If we create a new entry every 4096 bytes appended, brokers never read much more than 4KB plus one batch, regardless of how large the segment is. Since the index is never used as an authoritative source-of-truth, it doesn’t need careful persistence checks and can simply be rebuilt during recovery. 

A replica’s state file holds the current leader_epoch (4-byte integer) and the high watermark (8-byte integer), as well as a CRC checksum of the first 12 bytes (4 byte integer). When either value in this file is changed, the broker first creates a new temp file (state.tmp) and writes it to disk, then performs rename(“state.tmp”, “state”). On POSIX-compliant file systems (like Linux), this atomically replaces the old state file, ensuring that any crashes leave the broker with a complete state file on startup, either the old one or the new one. The checksum flags scenarios where this may not be the case (Windows, some WSL versions) and protects against disk corruption. On a failed check, HW falls back to 0.

- Crash recovery, for every partition at broker startup, prior to broker registering with Controller:
- List segments, open all, set newest segment to active
- Scan active segment and validate batch_length and CRC checksum
- Truncate file at first invalid batch (base offset != previous batch’s base offset + count)
- Rebuild active segment’s index from the scan and update LEO
- Load the state file, clamp HW to <= LEO

<img width="1840" height="1480" alt="replica_structure_background" src="https://github.com/user-attachments/assets/e50204da-3761-4868-81de-cfdfc818a3b9" />

## Current Development
> 🚧 **In active development.** The storage engine is complete; networking and the single-broker server are next. Code complete is planned for December 2026. Commands in the Quick start and Results sections describe the finished system.

- [x] **Project setup**
  - [x] Makefile build with unit tests and AddressSanitizer / ThreadSanitizer builds
  - [x] Verified on WSL, macOS, and Windows 11
  - [x] Cluster environment checks: local disk, open ports, passwordless SSH
- [ ] **Storage and a single broker**
  - [x] Record batches with CRC32C checksums and big-endian encoding
  - [x] Segment files, sparse offset index, and multi-segment partition log
  - [x] Crash recovery: torn writes truncated, verified with `kill -9` tests
  - [ ] Config files and structured event logging
  - [ ] Networking: sockets and length-prefixed framing
  - [ ] Wire protocol: Produce, Fetch, Metadata
  - [ ] Broker server and `produce` / `consume` command-line tools
- [ ] **Replication**
  - [ ] Topics, partitions, and key-based partitioning
  - [ ] Follower fetching, in-sync replica set, and high watermark
  - [ ] `acks=0/1/all` and `min_insync_replicas`
  - [ ] Load generator and durability checker
  - [ ] First multi-machine run
- [ ] **Failover**
  - [ ] Controller: broker registration, heartbeats, failure detection
  - [ ] Leader election from the ISR, with leader epochs and fencing
  - [ ] Client metadata refresh and retries
  - [ ] User-space fault injection (drop, delay, partition)
  - [ ] Checker passes with leaders killed in a loop
- [ ] **Consumer groups and demo**
  - [ ] Group membership, rebalancing, and generations
  - [ ] Committed offsets
  - [ ] Order-pipeline demo (`order_service`, `fulfillment`, `audit`)

