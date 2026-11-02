// 04.08: clang++-21 -std=c++20 -O1 -fsanitize=address,undefined dmatest.cpp -o dmatest && ./dmatest 3000
// Old vs new DmaPusher::ProcessCommands (04.08's SinkRun): the loop's logic copied from
// video_core/dma_pusher.cpp with the engines and puller mocked; every side effect is recorded
#include <bitset>
#include <cstdint>
#include <cstdio>
#include <random>
#include <span>
#include <string>
#include <vector>
using u32 = uint32_t; using u64 = uint64_t; using s64 = int64_t;
enum class SubmissionMode : u32 { IncreasingOld = 0, Increasing = 1, NonIncreasingOld = 2, NonIncreasing = 3, Inline = 4, IncreaseOnce = 5 };
struct CommandHeader {
    u32 argument;
    u32 method() const { return argument & 0x1fff; }
    u32 subchannel() const { return (argument >> 13) & 7; }
    u32 arg_count() const { return (argument >> 16) & 0x1fff; }
    u32 method_count() const { return (argument >> 16) & 0x1fff; }
    SubmissionMode mode() const { return SubmissionMode(argument >> 29); }
};
struct Engine { std::bitset<65535> execution_mask; std::vector<std::pair<u32, u32>> method_sink; };
struct Pusher {
    struct { u32 method, subchannel, method_count; u64 dma_get, dma_word_offset; bool non_incrementing, is_last_call; } dma_state{};
    bool dma_increment_once = false;
    Engine* subchannels[8]{};
    std::vector<std::string> log;
    static constexpr u32 non_puller_methods = 0x40;
    void SetState(const CommandHeader& h) { dma_state.method = h.method(); dma_state.subchannel = h.subchannel(); dma_state.method_count = h.method_count(); }
    void Consume(Engine* e) { for (auto [m, v] : e->method_sink) log.push_back("sink " + std::to_string(m) + "=" + std::to_string(v)); e->method_sink.clear(); }
    void CallMethod(u32 argument) {
        if (dma_state.method < non_puller_methods) {
            log.push_back("puller " + std::to_string(dma_state.method) + " " + std::to_string(argument) + " " + std::to_string(dma_state.subchannel) + " " + std::to_string(dma_state.method_count));
        } else {
            auto sc = subchannels[dma_state.subchannel];
            if (!sc->execution_mask[dma_state.method]) {
                sc->method_sink.emplace_back(dma_state.method, argument);
            } else {
                Consume(sc);
                log.push_back("exec " + std::to_string(dma_state.subchannel) + " " + std::to_string(dma_state.method) + " " + std::to_string(argument) + " last " + std::to_string(dma_state.is_last_call) + " seg " + std::to_string(dma_state.dma_get + dma_state.dma_word_offset));
            }
        }
    }
    void CallMultiMethod(const u32* base, u32 n, const u32* origin) {
        if (dma_state.method < non_puller_methods) {
            log.push_back("pmulti " + std::to_string(dma_state.method) + " " + std::to_string(base - origin) + " " + std::to_string(n) + " " + std::to_string(dma_state.method_count));
        } else {
            Consume(subchannels[dma_state.subchannel]);
            log.push_back("multi " + std::to_string(dma_state.subchannel) + " " + std::to_string(dma_state.method) + " " + std::to_string(base - origin) + " " + std::to_string(n) + " " + std::to_string(dma_state.method_count) + " seg " + std::to_string(dma_state.dma_get + dma_state.dma_word_offset));
        }
    }
    size_t SinkRun(std::span<const CommandHeader> commands, size_t index) {
        if (dma_state.method < non_puller_methods) return 0;
        Engine* const engine = subchannels[dma_state.subchannel];
        if (engine == nullptr) return 0;
        u32 method = dma_state.method; u32 count = dma_state.method_count; bool non_incrementing = dma_state.non_incrementing;
        size_t i = index; const size_t end = commands.size();
        while (count != 0 && i < end && !non_incrementing && !engine->execution_mask[method]) {
            engine->method_sink.emplace_back(method, commands[i].argument);
            ++i; --count; ++method; non_incrementing = dma_increment_once;
        }
        const size_t run = i - index;
        if (run != 0) {
            dma_state.dma_word_offset = u32((i - 1) * sizeof(u32)); dma_state.is_last_call = count == 0;
            dma_state.method = method; dma_state.method_count = count; dma_state.non_incrementing = non_incrementing;
        }
        return run;
    }
    template <bool New> void ProcessCommands(std::span<const CommandHeader> commands, const u32* origin) {
        for (size_t index = 0; index < commands.size();) {
            if (dma_state.method_count && dma_state.non_incrementing) {
                auto const& command_header = commands[index];
                dma_state.dma_word_offset = u32(index * sizeof(u32));
                const u32 max_write = u32(std::min<std::size_t>(index + dma_state.method_count, commands.size()) - index);
                CallMultiMethod(&command_header.argument, max_write, origin);
                dma_state.method_count -= max_write;
                dma_state.is_last_call = true;
                index += max_write;
            } else if (dma_state.method_count) {
                if constexpr (New) {
                    if (const size_t run = SinkRun(commands, index); run != 0) { index += run; continue; }
                }
                auto const command_header = commands[index];
                dma_state.dma_word_offset = u32(index * sizeof(u32));
                dma_state.is_last_call = dma_state.method_count <= 1;
                CallMethod(command_header.argument);
                dma_state.method += !dma_state.non_incrementing ? 1 : 0;
                dma_state.non_incrementing |= dma_increment_once;
                dma_state.method_count--;
                index++;
            } else {
                auto const command_header = commands[index];
                switch (command_header.mode()) {
                case SubmissionMode::Increasing: SetState(command_header); dma_state.non_incrementing = false; dma_increment_once = false; break;
                case SubmissionMode::NonIncreasing: SetState(command_header); dma_state.non_incrementing = true; dma_increment_once = false; break;
                case SubmissionMode::Inline:
                    dma_state.method = command_header.method(); dma_state.subchannel = command_header.subchannel();
                    dma_state.dma_word_offset = u64(-s64(dma_state.dma_get));
                    CallMethod(command_header.arg_count()); dma_state.non_incrementing = true; dma_increment_once = false; break;
                case SubmissionMode::IncreaseOnce: SetState(command_header); dma_state.non_incrementing = false; dma_increment_once = true; break;
                default: break;
                }
                index++;
            }
        }
    }
    std::string State() const {
        char b[256]; snprintf(b, sizeof b, "m%u s%u c%u off%llu ni%d last%d once%d", dma_state.method, dma_state.subchannel, dma_state.method_count,
                              (unsigned long long)dma_state.dma_word_offset, dma_state.non_incrementing, dma_state.is_last_call, dma_increment_once);
        std::string s = b;
        for (auto* e : subchannels) { s += " |"; for (auto [m, v] : e->method_sink) s += " " + std::to_string(m) + "=" + std::to_string(v); }
        return s;
    }
};
int main(int argc, char** argv) {
    const int seeds = argc > 1 ? atoi(argv[1]) : 2000;
    long words = 0, runs_differ = 0;
    for (int seed = 1; seed <= seeds; seed++) {
        std::mt19937 rng(seed);
        Engine ea[8], eb[8];
        const double exec_rate = (seed % 4) * 0.08;
        for (int s = 0; s < 8; s++) for (u32 m = 0; m < 0x4000; m++) { bool x = std::uniform_real_distribution<>(0, 1)(rng) < exec_rate; ea[s].execution_mask[m] = x; eb[s].execution_mask[m] = x; }
        Pusher a, b; for (int s = 0; s < 8; s++) { a.subchannels[s] = &ea[s]; b.subchannels[s] = &eb[s]; }
        std::vector<CommandHeader> stream;
        const int n = 50 + rng() % 400;
        while ((int)stream.size() < n) {
            // a header (the mode, a method, the subchannel, a count) and its data words
            u32 mode = std::vector<u32>{1, 1, 1, 3, 4, 5, 0, 2, 6, 7}[rng() % 10];
            u32 method = rng() % 3 == 0 ? rng() % 0x60 : 0x40 + rng() % 0x1fc0;
            u32 count = std::vector<u32>{0, 1, 2, 3, 5, 8, 16, 40}[rng() % 8];
            u32 sc = rng() % 8;
            stream.push_back({(mode << 29) | (count << 16) | (sc << 13) | method});
            if (mode != 4) for (u32 k = 0; k < count; k++) stream.push_back({u32(rng())});
        }
        // split into segments (command lists), as Step hands them over
        size_t pos = 0; u64 get = 0x1000;
        while (pos < stream.size()) {
            size_t len = std::min<size_t>(stream.size() - pos, 1 + rng() % 60);
            a.dma_state.dma_get = b.dma_state.dma_get = get;
            std::span<const CommandHeader> seg(stream.data() + pos, len);
            a.ProcessCommands<false>(seg, &stream[pos].argument);
            b.ProcessCommands<true>(seg, &stream[pos].argument);
            if (a.log != b.log || a.State() != b.State()) {
                printf("seed %d differs at word %zu\n  old %s\n  new %s\n", seed, pos, a.State().c_str(), b.State().c_str());
                for (size_t k = 0; k < std::max(a.log.size(), b.log.size()); k++) {
                    const char* x = k < a.log.size() ? a.log[k].c_str() : "-"; const char* y = k < b.log.size() ? b.log[k].c_str() : "-";
                    if (std::string(x) != y) { printf("  first log difference %zu: %s | %s\n", k, x, y); break; }
                }
                return 1;
            }
            pos += len; get += 0x400; words += len;
        }
    }
    printf("%d streams, %ld words: old and new loops equal (logs and states)\n", seeds, words);
    return 0;
}
