The quadratic fuzzer generates long sequences of repeated characters, such as `<?x<?x<?x<?x...`,
to detect quadratic complexity performance issues.

To build and run the quadratic fuzzer:

```bash
mkdir build-fuzz
cd build-fuzz
cmake -DMARKDOWN_CORE_FUZZ_QUADRATIC=ON -DCMAKE_C_COMPILER=$(which clang) -DCMAKE_CXX_COMPILER=$(which clang++) -DCMAKE_BUILD_TYPE=Release ..
make
../packages/markdown-core/fuzz/fuzzloop.sh
```
