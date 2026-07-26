Concepts behind [DD-002](../DD-002-buffer-pool-manager.md). See [[Concurrency-Control]] for the
locks/latches/sharding background this leans on.

## What a buffer pool actually is
- Disk is slow, memory is fast — a buffer pool is just a fixed-size in-memory cache of disk
  pages, so repeated access to the same page doesn't repeat the I/O.
- **Frame**: a fixed-size slot in the buffer pool that currently holds (or could hold) one
  page's worth of bytes, plus bookkeeping about that page. "Frame" and "page" are often
  conflated casually, but the distinction matters: a page is a disk-format concept
  (`PAGE_SIZE` bytes, identified by `page_id`), a frame is the in-memory *container* for one,
  identified by its `frame_id` (just its index in the pool array).
- The whole pool is typically one contiguous allocation (`capacity × sizeof(Frame)`) rather than
  one allocation per frame — better cache locality, and often required anyway once frames own
  non-movable things like mutexes/condvars.

## Page table
- A map `page_id → frame_id`: "if this page is currently cached, which frame is it in."
- Distinct from the **page directory** (on-disk `page_id → file offset`, DiskManager's concern)
  — the page table is purely in-memory and only exists to make cache lookups fast.
- Sharded in kernSQL (see [[Concurrency-Control]]) so lookups for unrelated pages don't
  serialize against each other.

## Pin count
- A page is "pinned" while some caller is actively using it — pin count is a reference count.
- ==Core invariant: a frame is only eligible for eviction when its pin count is zero.== Eviction
  candidacy is a direct function of this count, not a separate flag someone remembers to set.
- Pinning/unpinning is the mechanism that prevents "the buffer pool evicted a page while I still
  had a pointer into it" — a classic use-after-free class of bug in naive implementations.

## Dirty flag
- Set when a page's content has been modified since it was last written back to disk (or since
  it was loaded, if never written).
- Only dirty pages need a disk write on eviction/flush — clean pages can just be dropped, since
  disk already has an identical copy.
- Concurrency wrinkle: if two threads both hold a pin on the same frame and one modifies it, the
  dirty flag has to be **OR'd**, not overwritten — you can't let a clean unpinner race with a
  dirty unpinner and accidentally clear the flag on a page that's actually dirty because of the
  *other* pinner's write.

## Replacement policy (which page to evict on a miss with a full pool)
- **LRU (least recently used)**: evict the page that hasn't been touched longest. Great hit
  ratio in practice, but naive implementations need a doubly-linked list reordered on *every*
  access, which itself becomes a point of contention under concurrency — the exact convoy
  problem this whole design is trying to avoid.
- **Clock / second-chance** (what kernSQL uses): approximates LRU without reordering on every
  access. Each frame has a `usage_count` (or a single reference bit in the classic version); a
  "clock hand" sweeps candidate frames, decrementing/clearing the bit as it passes, and evicts
  the first frame it finds already at zero. A frame that was accessed gets its bit set again on
  next access, giving it a "second chance" before eviction.
  - Why this over strict LRU here: O(1) amortized, no reordering of a shared list on the hot
    read path — accesses only need to bump a counter (or set a bit) on *their own* frame, not
    touch a shared structure. That's a much smaller, more local piece of contention.
- **LRU-K / LRU-2**: tracks the *k*-th most recent access, not just the most recent — resists
  "one-off scan pollutes the cache" better than plain LRU (a single sequential scan touches many
  pages exactly once, which plain LRU would otherwise treat as "recently used" and protect from
  eviction ahead of genuinely hot pages). Not used here; noted as the thing you'd reach for if
  clock-sweep hit ratio proves insufficient in practice.

## Free list vs. Replacer — two different populations of frames
- **Free list**: frames that have *never* held a page yet, or were just vacated by an explicit
  delete. Nothing worth evicting — just an empty slot. Simple LIFO stack, fully populated at
  construction time.
- **Replacer**: frames that *currently hold a page* but are unpinned — i.e. real eviction
  candidates, tracked by the replacement policy above.
- A frame moves into the replacer's candidate set the instant its pin count drops to zero, and
  out of it the instant it's pinned again.
- ==Why bother with two structures instead of just running the replacement policy on
  everything?== Popping an empty free-list slot is unconditionally O(1) and touches nothing else
  — during pool warm-up (before every frame has held a page at least once), this avoids running
  a clock sweep at all. The miss path checks free list first, only consults the replacer once
  it's empty.

## Why frame metadata and frame *content* need separate protection
- Two very different operations touch a frame: "is this pinned, is it dirty, is it still
  loading" (nanoseconds) vs "read/write the actual page bytes" (can be held across a whole scan,
  potentially milliseconds).
- Protecting both with one latch means the fast metadata check can get stuck behind a slow
  content access — a convoy (see [[Concurrency-Control]]). Splitting them into a metadata
  mutex and a content R/W latch means a pin-count check never blocks on someone else's page
  scan.

## Loading flag — coordinating a cache miss without holding a latch across I/O
- ==A spin-or-short-block latch must never be held across a blocking syscall== — that would
  turn every other thread waiting on that latch into a thread effectively blocked on disk I/O,
  defeating the entire point of having fine-grained latches.
- Instead: a `loading` bool + condition variable per frame. The thread that causes the miss sets
  `loading = true`, releases all latches, performs the disk read *unlocked*, then re-acquires the
  metadata mutex, sets `loading = false`, and notifies waiters.
- Any other thread that looks up the same page while it's loading sees `loading == true`, and
  waits on the condvar instead of either (a) racing to issue a redundant read, or (b) reading
  stale/uninitialized content.

## What's deliberately *not* on the frame
- `frame_id` — purely positional (its own index in the pool array); storing a copy risks it
  drifting out of sync with reality for zero benefit.
- `page_lsn` — belongs to WAL/recovery, which doesn't exist yet in kernSQL. Adding the field
  now would just be dead weight until recovery design actually starts.
- `usage_count` — belongs to the *replacer's* bookkeeping, not the frame itself. Keeps "what
  page is this and how do I access it" (Frame's job) separate from "how do we decide what to
  evict" (Replacer's job) — lets the replacer be swapped out (LRU-K instead of clock, say)
  without touching Frame at all.

## Control flow shape (general pattern, not kernSQL-specific wording)
- **Hit path**: find frame via page table → bump pin count → tell the replacer this frame was
  just accessed / is no longer evictable → hand back a frame handle. Caller separately takes the
  content latch for whatever it actually wants to do.
- **Miss path**: get a free frame (free list, else evict via replacer) → if the victim is dirty,
  flush it first → update the page table (remove old mapping, insert new) → mark `loading` →
  read from disk *without* holding the page-table lock → clear `loading`, notify waiters.
- Inserting the new page-table mapping *before* releasing the lock (but before the read
  completes) matters: it's what makes a second concurrent fetcher of the *same* new page find
  the in-progress entry and wait on `loading`, instead of independently also triggering a second
  read of the same page.
- **Eviction of a dirty victim only needs a *shared* content latch**, not exclusive — because
  pin count already being zero means there's no active writer to race with; a flush is "just
  another reader" of the content.
