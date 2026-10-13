#include "prx/libSceVideoOut/include/CompileTitle.hpp"
#include <exception>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

CompileTitleInput running(std::uint64_t nowMs) {
    return CompileTitleInput{nowMs, 0, 47, 1, nowMs - 12300, 0, 0};
}

void hiddenWithoutCompiles() {
    require(!CompileTitleVisible(CompileTitleInput{50000, 0, 0, 0, 0, 0, 0}), "no compile has run");
}

void hiddenWhileFramesArrive() {
    auto input = running(50000);
    input.lastPresentMs = 49500;
    require(!CompileTitleVisible(input), "a frame 500 ms ago");
    input.lastPresentMs = 49000;
    require(!CompileTitleVisible(input), "a frame exactly 1 s ago");
    input.lastPresentMs = 48999;
    require(CompileTitleVisible(input), "no frame for more than 1 s");
}

void lingersAfterTheLastCompile() {
    CompileTitleInput input{50000, 0, 47, 0, 40000, 49000, 9000};
    require(CompileTitleVisible(input), "1 s after the last compile");
    input.nowMs = 50999;
    require(CompileTitleVisible(input), "just under 2 s after the last compile");
    input.nowMs = 51000;
    require(!CompileTitleVisible(input), "2 s after the last compile");
}

void formatsRunningCompile() {
    require(FormatCompileTitle("Sword of the Sea", running(50000)) == "Sword of the Sea | compiling shaders: 47 done, 12.3 s on the last one", "running compile");
}

void formatsFinishedCompile() {
    const CompileTitleInput input{50000, 0, 3, 0, 40000, 49000, 1300};
    require(FormatCompileTitle("T", input) == "T | compiling shaders: 3 done, 1.3 s on the last one", "finished compile");
}

void formatsKnownTotal() {
    auto input = running(50000);
    input.expected = 170;
    require(FormatCompileTitle("T", input) == "T | compiling shaders: 47 / ~170 done, 12.3 s on the last one", "known total");
}

void clockSkewDoesNotWrap() {
    const CompileTitleInput input{100, 200, 1, 1, 300, 0, 0};
    require(!CompileTitleVisible(input), "present after now");
    require(FormatCompileTitle("T", input) == "T | compiling shaders: 1 done, 0.0 s on the last one", "begin after now");
}

}

int main() {
    try {
        hiddenWithoutCompiles();
        hiddenWhileFramesArrive();
        lingersAfterTheLastCompile();
        formatsRunningCompile();
        formatsFinishedCompile();
        formatsKnownTotal();
        clockSkewDoesNotWrap();
    } catch (const std::exception& error) {
        std::cerr << "compile title test failed: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
