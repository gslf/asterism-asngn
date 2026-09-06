# Bounded acceptance and approval recovery

The runtime replays acceptance criteria and approvals one checked WAL frame at a
time. Acceptance frames are capped at 256 KiB; approval frames at 2 MiB. These
bounds cover their fixed field sizes, maximum criterion counts and escaped review
text. The generic WAL still enforces its 256 MiB log quota and verifies both
header and payload hashes. No runtime consumer materializes a whole framed WAL
as an xCDN document; whole-document inspection now exists only in test support.

Each complete frame must contain exactly one state value and match the canonical
UTF-8 bytes emitted by its typed codec. Comparing the codec's output, rather than
only the parser's object, is necessary because the pinned xCDN parser replaces
duplicate keys and represents strings as NUL-terminated C strings. Duplicates,
escaped NULs, reordered/extra annotations and other unwritten forms cannot become
accepted state through normalization. This is the internal persistent record
contract; it does not constrain ordinary user configuration formatting or replace
the xCDN parser. Previously written canonical records retain the same format.

Replay validates revisions, sequence numbers and immutable fields before
advancing the candidate state. Only a completely validated scan publishes the
recovered store. A complete invalid record stops replay without discarding it or
a later incomplete tail. A physically incomplete tail is repairable only after
valid preceding records. Snapshot refresh still determines whether stored proof
is current; canonical data alone never certifies a patch or makes an approval
replayable after interruption.

Acceptance tests reproduce two earlier failures: duplicate criteria fields were
normalized into accepted records, and semantic failure could follow a premature
tail repair. They also cover multiple states in one frame, escaped NULs, quota
rejection, the maximum 16-criterion definition and a history over 8 MiB with 257
exact state transitions. Approval tests include complete corruption, unwritten
metadata, a maximum 256 KiB review and a 256-record history over 32 MiB; see
[approval semantics](approvals.md).

These tests establish deterministic replay contracts. They do not measure total
process memory, prove power-loss durability on every filesystem, authorize
external effects, implement retention/compaction or resume interrupted actions.
