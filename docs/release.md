# Coordinated component builds

`release.json` pins sibling revisions and hashes their public headers. The engine
revision is the checked-out commit; `scripts/release.py` emits that exact revision
in the resolved manifest. Both CI jobs consume the same pins. The integrated job
also runs all sibling suites separately, including asmodel.

From the parent directory, after checking out the recorded revisions:

```sh
python3 asterism-asngn/scripts/release.py --root . > resolved-release.json
cmake -S asterism-asngn -B build -DASNGN_WITH_LLAMA=OFF
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

The verifier rejects dirty source or mismatched public headers. Its explicit
`--allow-engine-dirty` switch is only for development and reports `dirty: true`;
that output is not a release attestation. Public struct changes require an ABI
revision and a coordinated rebuild. asmodel/Asper ABI 3 expose runtime identifiers
checked by the host. Header hashes detect API drift within the pinned combination;
they are not a universal binary compatibility proof.

Publication is coordinated: publish the already-tested sibling commits first,
then the engine commit containing their manifest. Local commits are not evidence
that GitHub can fetch those revisions. Never replace pins with branch names to
make a failing checkout appear compatible.

The action/usage WAL uses schema 1 framing. Unframed older WALs are rejected; no
implicit migration can establish checksums for their original historical contents.
Keep a backup and use a fresh store for this development release. Conversation
projections and operation consumption have distinct semantics.
