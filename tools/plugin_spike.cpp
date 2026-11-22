// Spike for the plugin API design (design/plugin-api.md, section 10).
//
// It answers two questions with numbers instead of guesses:
//   1. What does the host-side packet dispatch table cost, for zero, one, five and twenty
//      hooks on the same packet id, with and without a payload copy?
//   2. What does a call into a wasm module cost, and what does the runtime itself cost to
//      find? The runtime is loaded at run time, the same way the worldgen device loads
//      OpenCL: nothing here is a build dependency.
//
// The two modules below are built here, byte by byte, so the spike carries no wasm file.
// Without a runtime on the machine they are only structurally checked and reported as not
// executed; the host numbers are still measured.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

    // --- runtime discovery ---------------------------------------------------------

    struct Runtime {
        void* library = nullptr;
        std::string name;
        std::string reason = "no WebAssembly runtime was found";
    };

    void* open_library(const char* name) {
#ifdef _WIN32
        return static_cast<void*>(::LoadLibraryA(name));
#else
        return ::dlopen(name, RTLD_NOW);
#endif
    }

    Runtime probe_runtime() {
        Runtime runtime;
        std::vector<std::string> candidates;
        if (const char* chosen = std::getenv("TERILA_WASM_RUNTIME")) {
            candidates.emplace_back(chosen);
        }
        for (const char* name : { "wasmtime.dll", "wasmer.dll", "wasm3.dll" }) {
            candidates.emplace_back(name);
        }
        for (const char* name : { "libwasmtime.so", "libwasmer.so", "libwasm3.so" }) {
            candidates.emplace_back(name);
        }
        std::string tried;
        for (const std::string& name : candidates) {
            if (void* library = open_library(name.c_str())) {
                runtime.library = library;
                runtime.name = name;
                runtime.reason.clear();
                return runtime;
            }
            tried += tried.empty() ? name : ", " + name;
        }
        runtime.reason += " (looked for " + tried + ")";
        return runtime;
    }

    // --- the two wasm modules, built by hand ---------------------------------------

    struct WasmWriter {
        std::vector<std::uint8_t> bytes;

        void u8(std::uint32_t value) { bytes.push_back(static_cast<std::uint8_t>(value)); }

        // Unsigned LEB128, which is what every wasm length and index uses.
        void u32(std::uint32_t value) {
            do {
                std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7F);
                value >>= 7;
                if (value != 0) {
                    byte = static_cast<std::uint8_t>(byte | 0x80);
                }
                bytes.push_back(byte);
            } while (value != 0);
        }

        void raw(const std::vector<std::uint8_t>& body) {
            bytes.insert(bytes.end(), body.begin(), body.end());
        }

        void section(std::uint8_t id, const std::vector<std::uint8_t>& body) {
            u8(id);
            u32(static_cast<std::uint32_t>(body.size()));
            raw(body);
        }

        void name(const std::string& text) {
            u32(static_cast<std::uint32_t>(text.size()));
            bytes.insert(bytes.end(), text.begin(), text.end());
        }
    };

    // A module exporting `entry(i32, i32) -> i32`, with one page of memory and no imports.
    // `body` is the instruction sequence of the export, terminated here by `end`.
    std::vector<std::uint8_t> build_module(const std::vector<std::uint8_t>& body,
        std::uint32_t locals, std::uint32_t pages) {
        WasmWriter writer;

        // Type: one function, two i32 parameters, one i32 result.
        WasmWriter types;
        types.u32(1);
        types.u8(0x60);
        types.u32(2);
        types.u8(0x7F);
        types.u8(0x7F);
        types.u32(1);
        types.u8(0x7F);

        // Function: one function of type zero.
        WasmWriter functions;
        functions.u32(1);
        functions.u32(0);

        // Memory: one memory, no maximum.
        WasmWriter memory;
        memory.u32(1);
        memory.u8(0x00);
        memory.u32(pages);

        // Export: `entry` as function zero.
        WasmWriter exports;
        exports.u32(1);
        exports.name("entry");
        exports.u8(0x00);
        exports.u32(0);

        // Code: local declarations, then the body and its `end`.
        WasmWriter code;
        code.u32(1);
        WasmWriter entry;
        entry.u32(locals == 0 ? 0 : 1);
        if (locals != 0) {
            entry.u32(locals);
            entry.u8(0x7F);
        }
        entry.raw(body);
        entry.u8(0x0B);
        code.u32(static_cast<std::uint32_t>(entry.bytes.size()));
        code.raw(entry.bytes);

        writer.u8(0x00);
        writer.u8(0x61);
        writer.u8(0x73);
        writer.u8(0x6D);
        writer.u8(0x01);
        writer.u8(0x00);
        writer.u8(0x00);
        writer.u8(0x00);
        writer.section(1, types.bytes);
        writer.section(3, functions.bytes);
        writer.section(5, memory.bytes);
        writer.section(7, exports.bytes);
        writer.section(10, code.bytes);
        return writer.bytes;
    }

    // Module `loop`: copies `len` bytes from address zero to address `dst` and returns 48.
    // Modules A, C and D of the design are this module called with a length of 0, 32 and
    // 65536, which is exactly the difference the spike wants to price.
    std::vector<std::uint8_t> build_copy_module(std::uint32_t pages) {
        const std::vector<std::uint8_t> body = {
            0x02, 0x40,             // block
            0x03, 0x40,             //   loop
            0x20, 0x01,             //     local.get 1  (len)
            0x45,                   //     i32.eqz
            0x0D, 0x01,             //     br_if 1
            0x20, 0x00,             //     local.get 0  (dst)
            0x20, 0x02,             //     local.get 2  (i)
            0x6A,                   //     i32.add
            0x20, 0x02,             //     local.get 2  (src)
            0x28, 0x02, 0x00,       //     i32.load align=2 offset=0
            0x36, 0x02, 0x00,       //     i32.store align=2 offset=0
            0x20, 0x02,             //     local.get 2
            0x41, 0x04,             //     i32.const 4
            0x6A,                   //     i32.add
            0x21, 0x02,             //     local.set 2
            0x20, 0x01,             //     local.get 1
            0x41, 0x04,             //     i32.const 4
            0x6B,                   //     i32.sub
            0x21, 0x01,             //     local.set 1
            0x0C, 0x00,             //     br 0
            0x0B,                   //   end
            0x0B,                   // end
            0x41, 0x30,             // i32.const 48
        };
        return build_module(body, 1, pages);
    }

    // Module `peek`: reads one header word and returns it, which is the cheapest possible
    // call that still touches guest memory.
    std::vector<std::uint8_t> build_peek_module() {
        const std::vector<std::uint8_t> body = {
            0x41, 0x00,             // i32.const 0
            0x28, 0x02, 0x00,       // i32.load align=2 offset=0
        };
        return build_module(body, 0, 1);
    }

    // Structure only: magic, version, and sections that fit inside the buffer. The spike
    // cannot do more than this without a runtime to hand the bytes to.
    bool looks_well_formed(const std::vector<std::uint8_t>& module) {
        if (module.size() < 8) {
            return false;
        }
        static const std::uint8_t magic[4] = { 0x00, 0x61, 0x73, 0x6D };
        if (std::memcmp(module.data(), magic, 4) != 0 || module[4] != 0x01) {
            return false;
        }
        std::size_t at = 8;
        while (at < module.size()) {
            ++at;  // section id
            std::uint32_t length = 0;
            int shift = 0;
            while (at < module.size()) {
                const std::uint8_t byte = module[at++];
                length |= static_cast<std::uint32_t>(byte & 0x7F) << shift;
                shift += 7;
                if ((byte & 0x80) == 0) {
                    break;
                }
            }
            if (at + length > module.size()) {
                return false;
            }
            at += length;
        }
        return true;
    }

    // --- the dispatch table ---------------------------------------------------------

    struct Hook {
        std::uint16_t id = 0;
        std::uint8_t direction = 0;
        std::uint8_t state = 0;
        std::uint16_t min_length = 0;
        std::uint16_t max_length = 0xFFFF;
        std::uint32_t flags = 0;
        std::uint32_t flags_mask = 0;
        bool observe = true;
        bool wants_payload = false;
    };

    struct Packet {
        std::uint16_t id = 0;
        std::uint8_t direction = 0;
        std::uint8_t state = 1;
        std::uint16_t length = 0;
        std::uint32_t flags = 0;
        std::vector<std::uint8_t> payload;
    };

    // Literals are tested cheapest and most selective first: id, then the direction and the
    // state word, then the length window, then the header flags. The first failure ends the
    // hook, which is what keeps a packet nobody subscribed to effectively free.
    inline bool matches(const Hook& hook, const Packet& packet) {
        if (packet.id != hook.id) {
            return false;
        }
        if (packet.direction != hook.direction || packet.state != hook.state) {
            return false;
        }
        if (packet.length < hook.min_length || packet.length > hook.max_length) {
            return false;
        }
        return (packet.flags & hook.flags_mask) == hook.flags;
    }

    struct Table {
        std::vector<Hook> slots[2][2][256];
    };

    struct Counters {
        std::size_t calls = 0;
        std::size_t bytes = 0;
    };

    // One dispatch, with the guest call replaced by the cost this spike can measure today:
    // a payload copy for the hooks that asked for one.
    Counters dispatch(const Table& table, const Packet& packet, std::uint8_t* scratch) {
        Counters counters;
        const std::vector<Hook>& hooks = table.slots[packet.direction][packet.state][packet.id];
        for (const Hook& hook : hooks) {
            if (!matches(hook, packet)) {
                continue;
            }
            ++counters.calls;
            if (hook.wants_payload && !packet.payload.empty()) {
                std::memcpy(scratch, packet.payload.data(), packet.payload.size());
                counters.bytes += packet.payload.size();
            }
        }
        return counters;
    }

    std::vector<Packet> packet_stream(std::size_t count) {
        std::vector<Packet> packets;
        packets.reserve(count);
        std::uint32_t rng = 0x12345678u;
        for (std::size_t i = 0; i < count; ++i) {
            rng = rng * 1664525u + 1013904223u;
            Packet packet;
            // One packet in eight is chat, the rest is spread over the neighbour ids so the
            // table lookup itself is exercised rather than one hot slot.
            packet.id = (rng >> 24) % 8 == 0 ? 0x08 : static_cast<std::uint16_t>(0x20 + (rng >> 20) % 32);
            packet.direction = static_cast<std::uint8_t>((rng >> 16) & 1);
            packet.state = 1;
            packet.length = static_cast<std::uint16_t>(16 + (rng >> 8) % 80);
            packet.flags = (rng >> 4) & 0x3;
            packet.payload.resize(packet.length);
            for (std::uint16_t at = 0; at < packet.length; ++at) {
                packet.payload[at] = static_cast<std::uint8_t>(at * 31 + i);
            }
            packets.push_back(std::move(packet));
        }
        return packets;
    }

    double nanoseconds_per_packet(const Table& table, const std::vector<Packet>& packets,
        std::uint8_t* scratch, std::size_t& calls, std::size_t& bytes) {
        constexpr int kRounds = 5;
        double best = 0.0;
        for (int round = 0; round < kRounds; ++round) {
            std::size_t round_calls = 0;
            std::size_t round_bytes = 0;
            const auto start = std::chrono::steady_clock::now();
            for (const Packet& packet : packets) {
                const Counters counters = dispatch(table, packet, scratch);
                round_calls += counters.calls;
                round_bytes += counters.bytes;
            }
            const double elapsed = std::chrono::duration<double, std::nano>(
                std::chrono::steady_clock::now() - start).count();
            const double each = elapsed / static_cast<double>(packets.size());
            if (best == 0.0 || each < best) {
                best = each;
                calls = round_calls;
                bytes = round_bytes;
            }
        }
        return best;
    }

    Table table_with(std::size_t hooks, bool wants_payload) {
        Table table;
        for (std::size_t i = 0; i < hooks; ++i) {
            Hook hook;
            hook.id = 0x08;
            hook.direction = 0;
            hook.state = 1;
            hook.min_length = static_cast<std::uint16_t>(16 + (i % 4) * 8);
            hook.max_length = 0xFFFF;
            hook.flags_mask = 0x1;
            hook.flags = static_cast<std::uint32_t>(i % 2);
            hook.observe = true;
            hook.wants_payload = wants_payload;
            table.slots[hook.direction][hook.state][hook.id].push_back(hook);
        }
        // Interceptors sit on the same id, so every scenario carries some of both.
        for (std::size_t i = 0; i < hooks / 2; ++i) {
            Hook hook;
            hook.id = 0x08;
            hook.direction = 0;
            hook.state = 1;
            hook.min_length = 24;
            hook.flags_mask = 0x2;
            hook.flags = 0;
            hook.observe = false;
            hook.wants_payload = wants_payload;
            table.slots[hook.direction][hook.state][hook.id].push_back(hook);
        }
        return table;
    }

    void report_copy_cost(std::uint8_t* scratch, std::vector<std::uint8_t>& source) {
        for (const std::size_t size : { std::size_t(32), std::size_t(65536) }) {
            source.assign(size, 0x5A);
            constexpr int kRounds = 20000;
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < kRounds; ++i) {
                std::memcpy(scratch, source.data(), size);
            }
            const double elapsed = std::chrono::duration<double, std::nano>(
                std::chrono::steady_clock::now() - start).count();
            std::printf("copy: %6zu B -> %8.1f ns each (%zu rounds)\n", size,
                elapsed / kRounds, std::size_t(kRounds));
        }
    }

} // namespace

int main() {
    // 1. The runtime, found at run time and never required to be there.
    const Runtime runtime = probe_runtime();
    if (runtime.library == nullptr) {
        std::printf("runtime: none - %s\n", runtime.reason.c_str());
    }
    else {
        std::printf("runtime: %s\n", runtime.name.c_str());
    }

    // 2. The modules. They are built here; with a runtime present they would be handed to it,
    //    and without one they are still checked for structural validity so the bytes are not
    //    unverified.
    const std::vector<std::uint8_t> copy_module = build_copy_module(2);
    const std::vector<std::uint8_t> peek_module = build_peek_module();
    std::printf("modules: loop=%zu bytes (valid=%s), peek=%zu bytes (valid=%s)\n",
        copy_module.size(), looks_well_formed(copy_module) ? "yes" : "no",
        peek_module.size(), looks_well_formed(peek_module) ? "yes" : "no");
    if (runtime.library == nullptr) {
        std::printf("modules: not executed - the wasm boundary and copy costs are pending a "
                    "runtime\n");
    }

    // 3. The dispatch table, over a stream of packets of which one in eight is chat.
    const std::vector<Packet> packets = packet_stream(200000);
    std::vector<std::uint8_t> scratch(65536 + 64);
    std::vector<std::uint8_t> source;

    std::printf("\nhost dispatch (200000 packets, 1 in 8 is the hooked id)\n");
    for (const std::size_t hooks : { std::size_t(0), std::size_t(1), std::size_t(5), std::size_t(20) }) {
        for (const bool payload : { false, true }) {
            const Table table = table_with(hooks, payload);
            std::size_t calls = 0;
            std::size_t bytes = 0;
            const double each = nanoseconds_per_packet(table, packets, scratch.data(), calls, bytes);
            // Calls and bytes are per thousand packets: one in eight is the hooked id, so a
            // per-packet average printed as an integer would only ever show a rounding.
            const double scale = 1000.0 / static_cast<double>(packets.size());
            std::printf("hooks=%2zu payload=%-3s -> %7.1f ns/packet (%5.1f calls, %6.1f bytes "
                        "per 1000 packets)\n",
                hooks, payload ? "yes" : "no", each, calls * scale, bytes * scale);
        }
    }

    // 4. What a payload copy costs on its own, which is what module C and D add to a call.
    std::printf("\n");
    report_copy_cost(scratch.data(), source);

    std::printf("\nverdict: filled into design/spike-results.md\n");
    return 0;
}
