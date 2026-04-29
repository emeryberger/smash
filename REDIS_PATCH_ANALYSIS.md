# Redis Smash-compatibility patches: analysis and future work

Analysis of the redis-smash patches applied to Redis 8.x to make it
cooperate with Smash's transparent heap compression. Ablation numbers
in §Evaluation are from the full EC2 run at source_hash
`a42226102017b6c9` (see `paper_results/linux/ablation_results.json`).

Upstream: [github.com/plasma-umass/redis-smash](https://github.com/plasma-umass/redis-smash),
commit `be660be` ("Add memory compressor compatibility (Smash)"),
7 files changed, +105 / −51.

## 1. The patches

### Patch 1: Write-through metadata cache on redisObject

Two new `uint32_t` fields on every `redisObject`: `cached_length` and
`cached_alloc_size`. The read paths `getObjectLength()`
(`object.c:926`) and `kvobjAllocSize()` (`object.c:1265`) compute
their values as before and then unconditionally write them back to
the cache:

```c
size_t getObjectLength(robj *o) {
    size_t len;
    switch (o->type) {
        case OBJ_STRING: len = stringObjectLen(o); break;
        case OBJ_LIST:   len = listTypeLength(o); break;
        case OBJ_SET:    len = setTypeSize(o); break;
        case OBJ_ZSET:   len = zsetLength(o); break;
        case OBJ_HASH:   len = hashTypeLength(o, 0); break;
        case OBJ_STREAM: len = streamLength(o); break;
        default: len = 0; break;
    }
    /* Write-through cache: every call updates cached_length for kvobjs,
     * so dbGenericDelete() can read it without touching value data pages. */
    if (o->iskvobj) o->cached_length = (uint32_t)len;
    return len;
}
```

Command handlers call these readers as part of normal execution
(58+ call sites), so the cache is always current for any key that
has ever been written. The delete path then reads the cache
instead of re-walking the value:

```c
/* db.c:852, dbGenericDelete */
int64_t oldlen = (int64_t) kv->cached_length;       /* was: getObjectLength(kv) */

/* db.c:887 */
updateSlotAllocSize(db, slot, kv, (size_t)kv->cached_alloc_size, -1);

/* lazyfree.c:32, populateDeltaHistograms (async free path) */
size_t len = kv->cached_length;                     /* was: getObjectLength(kv) */
```

**Why this matters.** `stringObjectLen(o)` walks into the SDS
header, which sits inside the value's data allocation.
`kvobjAllocSize()` sums up every sub-allocation for types with
internal structure (hash/zset trees). Both dereference `kv->ptr`,
which for a compressed page triggers `SIGSEGV` → decompression.
At 100K DELs the wholesale decompression churn is enough to keep
the entire value heap hot. With the cached fields in the object
header (which stays hot on its own regardless), DEL updates
histograms and frees async without touching value pages at all.

### Patch 2: idle-mode for serverCron

Adds three fields to `server`:

```c
int idle_mode;                        /* opt-in config */
long long idle_ticks;                 /* consecutive ticks with no commands */
long long last_idle_check_commands;   /* stat_numcommands snapshot */
```

Each cron tick compares `server.stat_numcommands` against the
last-seen value; if it hasn't moved for 3 consecutive ticks,
`is_idle = 1`. Then `databasesCron(is_idle)` gates three
data-page-touching operations:

```c
if (!is_idle) {
    activeDefragCycle();             /* walks all keys to compact */
    if (server.cluster_enabled)
        asmActiveTrimCycle();         /* cluster active-trim */
    if (!hasActiveChildProcess()) {
        kvstoreTryResizeDicts(...);   /* visits every slot's bucket chain */
        kvstoreIncrementallyRehash(...); /* migrates buckets */
    }
}
```

Everything else — `activeExpireCycle()`, `clientsCron()`,
`cronUpdateMemoryStats()`, replication/cluster heartbeats, AOF
persistence — runs unconditionally. Idle-mode only defers the
bookkeeping that would warm cold key/value pages.

The 3-tick latch adds a small amount of hysteresis so a momentary
command gap doesn't skip cron work mid-operation.

## 2. Why the two patches together unlock the 34.5% steady-RSS win

Without patch 1, DEL of 100K keys issues 100K `getObjectLength` +
`kvobjAllocSize` calls, each walking into a value page. Every one
of those is a SIGSEGV that decompresses the page just so Redis can
read a string length.

Without patch 2, every cron tick during the 20-second cool phase
runs `activeDefragCycle()`, which samples key allocations across
the keyspace. Smash would protect pages on the compressor tick;
`activeDefragCycle` would refault them; compressor's next tick
would protect again. A churn loop that keeps pages hot
indefinitely.

The ablation confirms they're complementary: the no-zero-deferred
(T2a) cross-check showed a patched-vs-unpatched gap of 5.8%
without zero-deferred vs 34.5% with it. All three mechanisms
together — zero-deferred, metadata cache, idle-mode — are
what unlock the benefit.

## 3. Evaluation (Linux, source_hash a42226102017b6c9, 3 runs)

Steady RSS (MiB, lower is better):

| Workload                    | Config | Unpatched | Patched |   Δ    |
|-----------------------------|--------|-----------|---------|--------|
| Standard (SET → cool → GET) | B0     |   264.1   |  261.4  | −1.0%  |
| Standard                    | B1     |   140.3   |  139.9  | **−0.3%** |
| Extended (+ DEL 50%)        | B0     |   263.7   |  261.7  | −0.8%  |
| Extended                    | B1     |   121.7   |   79.8  | **−34.5%** |

Headline readings:

- **No DELs → patches don't matter.** On the standard workload,
  patched and unpatched land within 0.3% of each other. The
  patches are latent; nothing exercises them.
- **With DELs → patches unlock a big Smash win.** Extended
  unpatched gets 59.5% RSS reduction; patched gets **73.1%** —
  an extra 13.6 pp, and 34.5% lower steady RSS in absolute terms.
- **B0 is unmoved by the patches**, which is the right sanity
  check: these are Smash-compatibility changes, not a memory
  optimization Redis could enjoy on its own.

## 4. Possible further patches

Ordered by expected impact.

### A — separate arena for value data (biggest structural win)

Currently every Redis allocation flows through `zmalloc` → Smash's
default arena hash (return-address-based). Headers and values for
a given key do end up in different arenas because their call sites
differ, but there's no deliberate policy — a value-creating call
from inside a module collides with a key-metadata allocation from
the same module.

**Change.** Expose `smash_alloc_in_arena(size, arena_id)` on the
Smash side. In Redis, route all SDS-value creation through a new
`sdsNewValue()` variant that targets `SMASH_VALUE_ARENA`. All
header/metadata allocations continue via the default path.

**Benefits.** (1) Value pages cluster, so Smash's per-origin
(arena × size-class) ROI statistics see a tight, homogeneous
ratio distribution — better compression-algorithm choice and
better amortization of calibration effort. (2) Header pages are
guaranteed cold-untouched by value allocation churn.
(3) Generalizes the "cache metadata in the header" idea (patch 1)
from object-level to arena-level segregation.

This is the most promising future-work bullet for the paper: it
extends an existing Smash primitive (arena routing) to be
application-directed rather than return-address-hashed.

### B — audit SCAN-family during idle

`SCAN`/`HSCAN`/`SSCAN`/`ZSCAN` iterate, and for small encodings
(listpack, intset, embstr ≤ 44 B) the entire value *is* the
pointer block. If a monitoring client does background SCANs
during otherwise-idle periods (Datadog, RedisInsight), those
touches would defeat cooling.

**Options.**
- Add a `scan-rate-limit` server option that throttles SCAN
  progress while `is_idle` is set.
- Have SCAN read cached type/length from the header and skip
  advancing into the value block when the caller only needs
  cursor state (this is a broader refactor).

### C — defer clientsCron's client-buffer shrinking

`clientsCron` calls `clientsCronResizeQueryBuffer` and
`clientsCronFreeArgvBuffer`, which read `client->buf` and
potentially reallocate. Client buffers are Smash-managed. If
clients are idle, these shrinks touch cold-ish pages. Add an
`is_idle` guard here too, deferring buffer shrinks to when
there's command traffic.

Small absolute win, but trivial to implement.

### D — explicit idle signal

The 3-tick hysteresis can miss short cool windows. Add a
`CLIENT IDLE-NOW` command that forces the idle latch. Useful for
workloads where the application can tell Redis "I'm done for now"
— end-of-batch, closed tracing span.

Convenience, not essential.

### E — cached serialized size for internal `MEMORY USAGE`

The user-facing `MEMORY USAGE <key>` command intentionally walks
the value (that's its job). But internal users of the same
computation — e.g., eviction candidate scoring in
`evictionPoolPopulate` — could read `cached_alloc_size` instead.

Small win; the big ones are in patch 1 already.

## 5. Non-opportunities (already handled)

- **`activeExpireCycle`.** Samples random keys from `db->expires`
  and checks TTL. The TTL is stored in `db->expires`'s hash (not
  in the value), so the TTL read stays cold-safe. On expiry hit,
  `dbGenericDelete` runs and already uses the cached fields from
  patch 1. Nothing more needed.
- **Keyspace notifications.** `notifyKeyspaceEvent()` formats a
  pub/sub message including the key name. Key name lives in
  `kvstoreDictEntry`, not the value. Already cold-safe.
- **Async DEL for all sizes.** `DEL` already dispatches to the
  lazyfree thread above a size threshold. Forcing async for small
  values as well might improve AUC but wouldn't change steady RSS.

## 6. Takeaway

The two patches are small (~150 lines net), semantics-preserving,
and close the two specific ways stock Redis broke transparent
compression: implicit value dereferences on delete, and
maintenance cron traversals during idle periods. The pattern
generalizes: any long-running server that caches derived metadata
in hot headers and gates background maintenance on an idle signal
will be compression-friendlier, regardless of whether the
underlying compressor is Smash, an OS-level page compressor, or
swap.

The most interesting follow-up is arena-level segregation of
value data (patch A above): it takes the object-level
"cache-in-the-header" idea and applies it at the allocator level,
which is where Smash's per-origin statistics can exploit it.
