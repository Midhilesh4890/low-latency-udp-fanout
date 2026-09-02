# Contributing

PulseFanout welcomes focused fixes, tests, documentation improvements, and measured performance work.

## Local workflow

1. Configure and build with CMake:

   ```bash
   cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
   cmake --build build --parallel
   ```

2. Run unit and integration tests:

   ```bash
   ctest --test-dir build --output-on-failure
   ./scripts/smoke_test.sh ./build/bin
   ```

3. Run a sanitizer build for changes near parsing, shared memory, or FEC:

   ```bash
   cmake -S . -B build-sanitize \
     -DCMAKE_BUILD_TYPE=Debug \
     -DPULSEFANOUT_ENABLE_SANITIZERS=ON
   cmake --build build-sanitize --parallel
   ctest --test-dir build-sanitize --output-on-failure
   ```

## Change expectations

- Keep the hot path allocation-free unless the change explicitly studies that tradeoff.
- Add regression tests for validation, sequence-window, ring, or FEC behavior changes.
- Include exact commands, hardware, topology, message count, and delivery gates with performance claims.
- Do not present a finite backlog-draining rate as sustainable throughput.
- Document wire-format changes and preserve compatibility or bump the protocol version.

CI builds with GCC and Clang and runs the localhost pipeline. Please keep commits small enough to review and explain any deliberate benchmark changes.
