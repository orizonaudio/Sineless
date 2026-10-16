// Runs a fuzz target over saved inputs without libFuzzer, as LLVM's StandaloneFuzzTargetMain.c
// does: each target builds with this main in every build that has tests, and ctest replays its
// seeds and the inputs of fixed crashes (regressions/<target>/) on every platform, so that a
// crash, a failed invariant or a sanitizer report on any of them fails the tests.
// Usage: <target> <file or directory>...   (directories are read whole, recursively)

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

// The target's entry point, as libFuzzer calls it.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace fs = std::filesystem;

int main(int argc, char** argv)
{
    std::vector<fs::path> inputs;
    for (int i = 1; i < argc; ++i) {
        const fs::path path(argv[i]);
        if (fs::is_directory(path)) {
            for (const auto& entry : fs::recursive_directory_iterator(path)) {
                if (entry.is_regular_file()) {
                    inputs.push_back(entry.path());
                }
            }
        } else if (fs::is_regular_file(path)) {
            inputs.push_back(path);
        } else {
            // A path that names nothing is a mistake in the test, not an input to skip.
            std::cerr << "No input at " << path.string() << "\n";
            return 1;
        }
    }
    if (inputs.empty()) {
        std::cerr << "No inputs to replay\n";
        return 1;
    }
    std::ranges::sort(inputs);
    for (const auto& path : inputs) {
        // Named before it runs, so that a crash says which input caused it.
        std::cerr << "Replaying " << path.string() << "\n";
        std::ifstream file(path, std::ios::binary);
        const std::vector<char> bytes{std::istreambuf_iterator<char>(file),
                                      std::istreambuf_iterator<char>()};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): libFuzzer's signature.
        LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    }
    std::cerr << "Replayed " << inputs.size() << " inputs\n";
    return 0;
}
