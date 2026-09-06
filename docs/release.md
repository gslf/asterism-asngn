# Coordinated component builds

`release.json` pins sibling revisions and hashes their public headers. The engine
revision is the checked-out commit; `scripts/release.py` emits that exact revision
in the resolved manifest. Engine CI and the real-model smoke workflow consume
the same pins. Asper's standalone jobs read `dependencies.json`; the release
checker rejects a dependency that differs from the coordinated asmodel pin.
The integrated job runs all sibling suites separately. Both it and standalone
asmodel CI require the HTTP conformance target, so a missing test dependency
cannot silently remove provider coverage.

From the parent directory, after checking out the recorded revisions:

```sh
python3 asterism-asngn/scripts/release.py --root . > resolved-release.json
cmake -S asterism-asngn -B build -DASNGN_WITH_LLAMA=OFF
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

The verifier checks required xCDN working checkouts as well as their gitlinks;
an initialized llama.cpp checkout must also be clean and match its pin. Pass
`--with-llama` to require that native checkout, as the real-model smoke does.
Unused, uninitialized llama.cpp is reported explicitly in a no-llama release.

The verifier rejects dirty source or mismatched public headers. Its explicit
`--allow-engine-dirty` switch is only for development and reports `dirty: true`;
that output is not a release attestation. Public struct changes require an ABI
revision and a coordinated rebuild. The host checks each linked sibling's runtime
ABI identifier against its compiled header. Header hashes detect API drift within the pinned combination;
they are not a universal binary compatibility proof.

Publication is coordinated: publish the already-tested sibling commits first,
then the engine commit containing their manifest. Local commits are not evidence
that GitHub can fetch those revisions. Never replace pins with branch names to
make a failing checkout appear compatible.

The action/usage WAL uses version 2 framing with separate header and payload
checksums. Older WAL formats are rejected; no
implicit migration can establish checksums for their original historical contents.
Keep a backup and use a fresh store for this development release. Conversation
projections and operation consumption have distinct semantics.
