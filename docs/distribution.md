# Local Linux runtime distribution

The `linux-remote` profile builds a relocatable runtime archive from the four
clean local component pins. It contains the TUI/headless CLI, three MCP servers,
the Astools checker/jail helper, standard tool packages, starter configurations
and licenses. It contains no weights, secrets, session state or development
libraries. It uses the host Linux C/C++ runtime and libcurl dependencies;
relocation on the build host does not prove portability to another distribution.

From a coordinated checkout with Python 3.11+, CMake/CPack and a C compiler:

```sh
python3 asterism-asngn/scripts/package.py --root . --output /new/runtime-build
```

This command verifies pins, clones them locally into an isolated temporary build,
builds a release profile with the embedded backend disabled, installs and
relocates the result, and exercises `doctor` plus a real strict-sandbox tool call.
The actual archive is also extracted and exercised before publication to the
local output directory. It then emits a `.tar.gz`, SHA-256 checksum, build log, installation test report
and a build receipt containing the exact source revisions. No downloads,
inference, publication, privileged install or modification of an existing
installation occurs. Choose a new output directory.

These artifacts are **unsigned development builds**. A checksum detects changed
archive bytes; it does not authenticate a publisher. Signing and cross-platform
release validation remain separate release requirements. A successful package
test is not evidence of model behavior or full provider conformance.

Extract the archive into a new directory, then invoke `bin/asngn --doctor`.
Without model configuration this must report `Preflight: action required` and
must not create engine state. Configure a real provider using the included
`share/asterism/examples` documentation; a remote endpoint/model and any required
credential must be supplied. The current archive does not provide offline
inference. Keep writable engine state separate from the extracted installation.
Configure the Astools registry to use the absolute
`share/asterism/tools` path, or copy those packages into a new engine root's
`tools` directory as shown in the examples. The jail helper lives beside the
executables so relocation retains Linux strict sandbox discovery.

For local development installation without a clean release claim:

```sh
cmake -S asterism-asngn -B build-runtime \
  -DASNGN_WITH_LLAMA=OFF -DASNGN_BUILD_DISTRIBUTION=ON
cmake --build build-runtime -j4
ctest --test-dir build-runtime -R '^test_distribution$' --output-on-failure
cmake --install build-runtime --prefix /new/installation
```

The distribution option requires Linux, threaded TUI/MCP targets and no
sanitizers. Other build profiles remain available for development and embedding.
`cmake --install` has CMake's ordinary replacement semantics; select a new prefix
for reviewable upgrades. Existing stores are not migrated or erased by packaging.
Clean pin checks apply to `scripts/package.py`, not an arbitrary CPack invocation.
