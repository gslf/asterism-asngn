# Workspace enumeration and snapshot boundaries

Fingerprinting and background repository retrieval share one directory walker,
with a common ignore policy, deterministic UTF-8 name ordering and global entry
quotas. File callbacks borrow already opened regular files. Neither consumer
reconstructs a pathname and follows it after enumeration.

On POSIX, enumeration uses directory descriptors, `fstatat` without following
links, and `openat` with `O_NOFOLLOW`. The opened identity must match the observed
entry. File and directory metadata are checked again after their callbacks.
Observable concurrent modifications return `ASNGN_ERR_BUSY`, including when a
consumer reaches its corpus limit.

The Windows implementation enumerates directory handles with
[GetFileInformationByHandleEx](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getfileinformationbyhandleex)
and bounds the returned
[directory entries](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_id_both_dir_info).
Parent handles omit delete sharing while traversal is active, and reparse points
are excluded. This implementation has not been built or executed on Windows in
the current Linux validation run; Windows/filesystem enforcement remains a gate.

## Limits and incomplete scans

The default snapshot admits at most 65,536 directory entries, 64 nested levels,
8 MiB per file and 256 MiB of total file content. Entries count directories and
excluded objects too. Name storage is bounded to 16 MiB and relative paths to
4,095 UTF-8 bytes. Quotas are checked during enumeration and before file callbacks,
including within one large flat directory.

Fingerprinting hashes files through an 8 KiB buffer. Version 2 binds ordered paths,
fixed-width little-endian sizes and per-file SHA-256 values alongside workspace
identity. Failed/incomplete scans clear the fingerprint and leave census data
unavailable. An excluded alias or special file prevents certification of the
workspace, even though the walker can safely report its other regular files.
Old fingerprint values naturally invalidate existing proof/cache dependencies.

Ignored directory names are `.git`, `.hg`, `.svn`, `.asterism`, `node_modules`,
`build`, `out`, `dist`, `target` and `__pycache__`. Matching is case-sensitive.
Ordinary files named `build` remain visible. Hidden configuration directories and
dependency source directories are no longer silently omitted by retrieval alone.
The active file follows the same background indexing ignore policy.

`.gitignore` and `.asterismignore` contents influence snapshot identity, but their
patterns are **not yet interpreted**. The workspace metadata states this limited
policy explicitly. Full ignore syntax and incremental reconciliation remain work.

## Git metadata

Snapshot identity now uses a separate bounded reader for `HEAD`, loose references
and `packed-refs`. On POSIX, every path component is opened without following
symlinks, including ancestors of metadata outside a linked worktree. Special
files are rejected. Reference names are validated before opening them; binary,
multiline and invalid object-ID data cannot become a reported commit. Branch names
retain their hierarchy and are rejected beyond 127 bytes instead of truncated.

Metadata lines admit 1 KiB, packed references 4 MiB and symbolic reference chains
eight hops. Duplicate packed entries for the selected reference fail. Git identity
is read before and after the source walk; an observed change rejects the snapshot.
As with the source walk, this is not an atomic snapshot or protection against ABA
changes. A syntactically valid OID is an observation, not proof of the commit's
existence, authenticity or object contents.

Ordinary `.git` directories and registered linked worktrees are supported. An
ancestor's empty `.git` directory does not establish a repository boundary. A
linked worktree's back-pointer must identify this checkout, and its `commondir`
must match the parent of `worktrees/<id>`. Common and per-worktree references follow
the [Git repository layout](https://git-scm.com/docs/gitrepository-layout).
These registration checks deliberately constrain external metadata access; they
are not multi-user access control. Arbitrary `gitdir` redirects, separate Git
directories, selected submodule Git files, unregistered common directories and
reftable storage return `UNSUPPORTED` instead of guessing. Git environment
overrides are not inherited, and the reader does not run Git, hooks or config.
Submodule source inside a selected parent checkout still uses normal tree scanning.

Nine Linux metadata cases cover aliases, a FIFO, traversal references, binary and
oversized data, cycles, duplicate refs, empty ancestor markers and damaged worktree registration. A real
Git integration compares observed IDs for SHA-1 and SHA-256 repositories through
worktree commits, packing and detached HEAD. A probe against `22a8101` returned
external fixture bytes as a commit and produced a fingerprint for
`HEAD = ref: ../../external`; the corrected reader returns `PARSE` with no identity
or fingerprint. Windows metadata behavior has not been executed in this run.

## Retrieval evidence

Retrieval retains at most 1,024 chunks of about 2 KiB each from files of at most
256 KiB. Its streaming selector continues walking after that resident corpus is
full. It considers at most 64 MiB of successfully captured content per scan,
including the initial active-file read and its recheck. Oversized/non-code files
contribute metadata only. Files that exceed the remaining content allowance are
skipped, while bounded metadata enumeration continues. A probe of an oversized
active file can read one byte beyond its per-file cap to detect the overflow.

The active file is admitted first and its chunks are pinned. Every other file
contributes at most eight candidates to a bounded global heap. Candidates match
case-sensitive whole identifiers and path terms from the current retrieval query;
repeated occurrences do not increase the score. At most 64 distinct terms of
2..191 UTF-8 bytes are considered. The score is distinct path/content term coverage,
plus twice the path coverage and 64 for a literal full-path mention. Ties prefer
ordered paths and earlier byte ranges. These are interpretable, **uncalibrated**
admission rules, separate from the final hybrid ranker and its six-result budget.
They do not provide semantic symbol resolution, language normalization or a
measured role-coverage guarantee.

This keeps an early large file from occupying the whole corpus and permits later
matching files or late matching ranges to displace weaker evidence. Only retained
candidates allocate text/path copies. Heap admission costs logarithmic time in the
resident corpus; the per-file shortlist has eight entries. Source scanning can
cost more than the previous early exit; the byte budget, cancellation and remaining
turn deadline bound this work. No end-to-end latency improvement is claimed.

Each selected chunk carries the whole-file hash, chunk hash, line range and byte
range. The active source is rechecked even for a non-code extension; changed,
binary, removed or unobserved active content cannot leave a stable admitted view.
A metadata-limited traversal that could not recheck it returns `BUSY`. These are
observed versions, not atomic snapshots or semantic symbol identities.

Schema-2 `retrieval_scan` telemetry reports visited entries/files, captured bytes,
read operations, candidate/retained/pinned counts, skipped large/binary/budget
files and query-term truncation. `traversal_complete` describes the walker;
`complete` additionally requires no eligible evidence to be dropped. A completed
walk can therefore return `LIMIT` and usable partial evidence. I/O errors,
observed conflicts and cancellation still reject the new index. Neither field
means all evidence needed by a task was retrieved. Source remains untrusted data.

Six retrieval cases cover shared ignores and source versions, active admission,
a late file after the former 1,024-chunk cutoff, global capacity with late Unicode
matches and exact ranges, stale active bytes/cancellation/deadline, and a 64 MiB
content limit with the active recheck both before and after budget exhaustion.
The late-file case fails against `6974045` and passes with streaming admission.
The integrated embedding fixture now adds nine early 240 KiB files and still
includes the late matching document in its single bounded embedding batch.
An empty eligible corpus now performs no query or document embedding call.
This is a controlled recall regression, not a real-repository task benchmark.

## Evidence and remaining work

Nine filesystem cases pass on Linux with sanitizers, covering flat/nested
quotas, cancellation, changed files/directories, renamed ancestors, external
aliases and a FIFO, and content/rename/delete fingerprints. Retrieval cases now
run in their own executable.

A separate production-limit reproduction used the same 33 sparse files of 8 MiB
against both implementations. Commit `d45c3fb` accepted 264 MiB and produced a
fingerprint despite the 256 MiB limit; the new implementation returns `LIMIT`
without a fingerprint. This is a correctness result, not a performance benchmark.

This remains a full scan, not an incremental snapshot service. A directory walk
is not an atomic filesystem snapshot: edits after a file's read, coarse filesystem
timestamps, remote filesystems, hard links and external build dependencies require
additional coordination. Watchers, cached inventories, editor buffers and isolated
job worktrees remain separate milestones. No universal sandbox or power-loss
guarantee follows from these tests.
