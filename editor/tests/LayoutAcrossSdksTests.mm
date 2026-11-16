// A host built against an older macOS SDK lays the editor out as a current host does (#424).
// WebKit keeps some layout behaviour for apps linked against older SDKs, keyed to the host's main
// executable: before the macOS 13.3 SDK it gives form controls a legacy margin, which set the bars'
// controls apart in REAPER 7.83 (SDK 11.0). Two builds of the layout probe walk the page through
// the same states, one linked as SDK 11.0 and one as the current SDK; every box must match.

#include "MacPageTesting.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <future>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct ProbeRun
{
    int status = -1;
    std::string output;
};

// Runs a probe and reads what it prints. Its standard error, where WebKit logs, is left out: a log
// line among the boxes would read as a difference.
ProbeRun runProbe(const char* probe)
{
    const std::string command =
        std::string("'") + probe + "' '" ORIZON_UI_DIST "' '" ORIZON_LAYOUT_WALK "' 2>/dev/null";
    ProbeRun run;
    // NOLINTNEXTLINE(bugprone-command-processor): the test runs its own probe, built with it.
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return run;
    }
    std::array<char, 65536> buffer{};
    std::size_t read = 0;
    while ((read = std::fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
        run.output.append(buffer.data(), read);
    }
    run.status = pclose(pipe);
    return run;
}

std::vector<std::string> linesOf(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);) {
        lines.push_back(line);
    }
    return lines;
}

std::string lineStarting(const std::vector<std::string>& lines, const std::string& prefix)
{
    for (const auto& line : lines) {
        if (line.starts_with(prefix)) {
            return line;
        }
    }
    return "";
}

// Where the walk could not run, the line saying why: "stalled ..." or "failed ...".
std::string trouble(const std::vector<std::string>& lines)
{
    const std::string stalled = lineStarting(lines, "stalled ");
    return stalled.empty() ? lineStarting(lines, "failed") : stalled;
}

// An area of the page sized by what it shows: the keys its boxes go by, and the text it showed.
struct Varying
{
    std::vector<std::string> keys;
    std::string text;
};

// One step of the walk: its name (with the editor width it ran at), the areas sized by what they
// showed, and every element's box.
struct Step
{
    std::string name;
    std::vector<Varying> varying;
    std::vector<std::string> boxes;
};

// "varies <key>,<key>... <text>"
Varying varyingOf(const std::string& line)
{
    const std::string rest = line.substr(std::string("varies ").size());
    const std::size_t space = rest.find(' ');
    Varying varying;
    std::istringstream keys(rest.substr(0, space));
    for (std::string key; std::getline(keys, key, ',');) {
        varying.keys.push_back(key);
    }
    varying.text = space == std::string::npos ? "" : rest.substr(space + 1);
    return varying;
}

// Whether a box line is of the element `key` or of one inside it with no id of its own.
bool boxOf(const std::string& line, const std::string& key)
{
    return line.starts_with(key) && line.size() > key.size() &&
           (line[key.size()] == ' ' || line[key.size()] == '>');
}

std::vector<Step> stepsOf(const std::vector<std::string>& lines)
{
    std::vector<Step> steps;
    std::string width;
    for (const auto& line : lines) {
        if (line.starts_with("width ")) {
            width = line;
        } else if (line.starts_with("step ")) {
            std::string name = width;
            name.append(", ").append(line);
            steps.push_back({.name = name, .varying = {}, .boxes = {}});
        } else if (line.starts_with("varies ") && !steps.empty()) {
            steps.back().varying.push_back(varyingOf(line));
        } else if (!steps.empty() && line != "done") {
            steps.back().boxes.push_back(line);
        }
    }
    return steps;
}

TEST(LayoutAcrossSdksTest, MatchesInAHostBuiltAgainstMacOS11)
{
    // Both at once: each is its own process with its own web content process.
    auto oldFuture =
        std::async(std::launch::async, [] { return runProbe(ORIZON_LAYOUT_PROBE_OLD_SDK); });
    const ProbeRun current = runProbe(ORIZON_LAYOUT_PROBE);
    const ProbeRun old = oldFuture.get();
    const auto currentLines = linesOf(current.output);
    const auto oldLines = linesOf(old.output);

    // A walk whose frames stopped because the display stopped driving them: CI's virtual display
    // does now and then (MacPageTesting.h); anywhere else it fails.
    for (const auto* lines : {&currentLines, &oldLines}) {
        const std::string stall = lineStarting(*lines, "stalled ");
        if (!stall.empty() && stall.ends_with(" stalled")) {
            if (orizon::testing::onCi()) {
                GTEST_SKIP() << "The display stopped driving the page's frames: " << stall;
            }
            FAIL() << "The page's frames stopped: " << stall;
        }
    }
    ASSERT_EQ(current.status, 0) << trouble(currentLines) << "\n" << current.output.substr(0, 2000);
    ASSERT_EQ(old.status, 0) << trouble(oldLines) << "\n" << old.output.substr(0, 2000);

    // The comparison is only worth something where the old probe shows the old behaviour: the
    // legacy margin on an unstyled control. Without it, the test no longer reproduces an old host.
    EXPECT_EQ(lineStarting(currentLines, "canary "), "canary 0px 0px");
    ASSERT_NE(lineStarting(oldLines, "canary "), "canary 0px 0px")
        << "The probe linked as macOS 11.0 shows no legacy form control margin: "
        << lineStarting(oldLines, "canary ");

    // Step by step, element by element; the first few differences of each step are named.
    const auto currentSteps = stepsOf(currentLines);
    const auto oldSteps = stepsOf(oldLines);
    ASSERT_FALSE(currentSteps.empty()) << "The walk measured nothing";
    ASSERT_EQ(currentSteps.size(), oldSteps.size()) << "The walks took different steps";
    int differences = 0;
    for (std::size_t s = 0; s < currentSteps.size(); ++s) {
        const Step& now = currentSteps[s];
        const Step& then = oldSteps[s];
        ASSERT_EQ(now.name, then.name) << "The walks went different ways";
        // The areas sized by what they show change with where the mock's sweep is when the step
        // is measured, so each is compared only where both show the same text.
        ASSERT_EQ(now.varying.size(), then.varying.size()) << now.name;
        std::vector<std::string> skipped;
        for (std::size_t v = 0; v < now.varying.size(); ++v) {
            if (now.varying[v].text != then.varying[v].text) {
                skipped.insert(skipped.end(), now.varying[v].keys.begin(),
                               now.varying[v].keys.end());
            }
        }
        const auto compared = [&](const std::vector<std::string>& lines) {
            std::vector<std::string> kept;
            for (const auto& line : lines) {
                if (std::ranges::none_of(
                        skipped, [&](const std::string& key) { return boxOf(line, key); })) {
                    kept.push_back(line);
                }
            }
            return kept;
        };
        const auto a = compared(now.boxes);
        const auto b = compared(then.boxes);
        if (a == b) {
            continue;
        }
        ++differences;
        int named = 0;
        for (std::size_t i = 0; i < std::max(a.size(), b.size()) && named < 5; ++i) {
            const std::string x = i < a.size() ? a[i] : "(none)";
            const std::string y = i < b.size() ? b[i] : "(none)";
            if (x != y) {
                ++named;
                ADD_FAILURE() << now.name << "\n  current SDK: " << x << "\n  macOS 11.0:  " << y;
            }
        }
    }
    EXPECT_EQ(differences, 0) << "steps laid out differently in a host built against macOS 11.0";
}

} // namespace
