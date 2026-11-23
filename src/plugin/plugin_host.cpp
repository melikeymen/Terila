#include "plugin/plugin_host.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "log/logger.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// The runtime is loaded at run time, exactly like the worldgen device's OpenCL loader, so a
// build without the C API header still starts and still enforces the gate. Only when the
// spike/host is compiled with TERILA_HAVE_WASMTIME is there a wasm side at all.
#if defined(TERILA_HAVE_WASMTIME)
#include <wasmtime.h>
#endif

namespace terila::plugin {

    namespace {

        constexpr const char* kManifestFile = "plugin.json";
        constexpr const char* kRequestFile = "permissions.json";

        bool read_json(const std::filesystem::path& path, nlohmann::json& out,
            std::string& error) {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                error = "cannot be read";
                return false;
            }
            try {
                in >> out;
            }
            catch (const std::exception& failure) {
                error = failure.what();
                return false;
            }
            if (out.is_discarded()) {
                error = "is not valid JSON";
                return false;
            }
            return true;
        }

        std::string refused(const Problem& problem) {
            return "refused: " + problem.text();
        }

        // The chat sink is the host's one way out to the players; the server installs it and
        // the plugin host only knows that a line of text leaves. It lives outside the wasm
        // side so the gate stays buildable without a runtime.
        std::function<bool(std::int32_t, std::string_view)>& chat_sink() {
            static std::function<bool(std::int32_t, std::string_view)> sink;
            return sink;
        }

        // Same for the player list: the host knows only that somebody can enumerate the online
        // players and read one of them by index.
        PlayerSource& player_source() {
            static PlayerSource source;
            return source;
        }

#if defined(TERILA_HAVE_WASMTIME)

        // What one plugin's host functions share: its id for prefixes and its share of the
        // event budget. The design budgets the plugin, not the function, so every host
        // function of a plugin draws from this one window. The slot owns the record and the
        // runtime is handed the pointer as the function environment.
        struct HostCall {
            std::string id;
            std::int32_t per_second = 0;
            std::int32_t used = 0;
            std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();

            bool take() {
                if (per_second <= 0) return true;
                const auto now = std::chrono::steady_clock::now();
                if (now - window >= std::chrono::seconds(1)) {
                    window = now;
                    used = 0;
                }
                if (used >= per_second) return false;
                ++used;
                return true;
            }
        };

        struct Slot {
            wasmtime_store_t* store = nullptr;
            wasmtime_instance_t instance{};
            std::int32_t fuel = kDefaultFuelPerCall;
            std::int32_t traps = 0;
            // Outlives every host function that points at it: the store is closed first.
            std::unique_ptr<HostCall> call;
        };

        // Loaded plugins own one store each, so a runaway plugin cannot disturb another one's
        // fuel or state. The instance is a handle and is kept by value, so there is no
        // allocation of its own that could be lost.
        std::map<std::string, Slot>& slots() {
            static std::map<std::string, Slot> store;
            return store;
        }

        template <typename Fn>
        Fn symbol(void* library, const char* name) {
#ifdef _WIN32
            return reinterpret_cast<Fn>(::GetProcAddress(static_cast<HMODULE>(library), name));
#else
            return reinterpret_cast<Fn>(::dlsym(library, name));
#endif
        }

        // The part of the C API the host uses. Resolved by name so nothing is linked in.
        struct WasmApi {
            decltype(&::wasm_engine_new) engine_new = nullptr;
            decltype(&::wasm_engine_delete) engine_delete = nullptr;
            decltype(&::wasm_config_new) config_new = nullptr;
            decltype(&::wasmtime_config_consume_fuel_set) consume_fuel_set = nullptr;
            decltype(&::wasm_engine_new_with_config) engine_new_with_config = nullptr;
            decltype(&::wasmtime_store_new) store_new = nullptr;
            decltype(&::wasmtime_store_delete) store_delete = nullptr;
            decltype(&::wasmtime_store_context) store_context = nullptr;
            decltype(&::wasmtime_module_new) module_new = nullptr;
            decltype(&::wasmtime_module_delete) module_delete = nullptr;
            decltype(&::wasmtime_instance_export_get) export_get = nullptr;
            decltype(&::wasmtime_func_call) func_call = nullptr;
            decltype(&::wasmtime_context_set_fuel) context_set_fuel = nullptr;
            decltype(&::wasmtime_error_delete) error_delete = nullptr;
            decltype(&::wasmtime_error_message) error_message = nullptr;
            decltype(&::wasm_trap_delete) trap_delete = nullptr;
            decltype(&::wasm_valtype_new) valtype_new = nullptr;
            decltype(&::wasm_valtype_vec_new) valtype_vec_new = nullptr;
            decltype(&::wasm_functype_new) functype_new = nullptr;
            decltype(&::wasm_functype_delete) functype_delete = nullptr;
            decltype(&::wasm_byte_vec_delete) byte_vec_delete = nullptr;
            decltype(&::wasmtime_linker_new) linker_new = nullptr;
            decltype(&::wasmtime_linker_define_func) linker_define_func = nullptr;
            decltype(&::wasmtime_linker_instantiate) linker_instantiate = nullptr;
            decltype(&::wasmtime_linker_delete) linker_delete = nullptr;
            decltype(&::wasmtime_caller_context) caller_context = nullptr;
            decltype(&::wasmtime_caller_export_get) caller_export_get = nullptr;
            decltype(&::wasmtime_memory_data) memory_data = nullptr;
            decltype(&::wasmtime_memory_data_size) memory_data_size = nullptr;

            bool resolve(void* library) {
                engine_new = symbol<decltype(engine_new)>(library, "wasm_engine_new");
                engine_delete = symbol<decltype(engine_delete)>(library, "wasm_engine_delete");
                config_new = symbol<decltype(config_new)>(library, "wasm_config_new");
                consume_fuel_set = symbol<decltype(consume_fuel_set)>(library,
                    "wasmtime_config_consume_fuel_set");
                engine_new_with_config = symbol<decltype(engine_new_with_config)>(library,
                    "wasm_engine_new_with_config");
                store_new = symbol<decltype(store_new)>(library, "wasmtime_store_new");
                store_delete = symbol<decltype(store_delete)>(library, "wasmtime_store_delete");
                store_context = symbol<decltype(store_context)>(library,
                    "wasmtime_store_context");
                module_new = symbol<decltype(module_new)>(library, "wasmtime_module_new");
                module_delete = symbol<decltype(module_delete)>(library, "wasmtime_module_delete");
                export_get = symbol<decltype(export_get)>(library,
                    "wasmtime_instance_export_get");
                func_call = symbol<decltype(func_call)>(library, "wasmtime_func_call");
                context_set_fuel = symbol<decltype(context_set_fuel)>(library,
                    "wasmtime_context_set_fuel");
                error_delete = symbol<decltype(error_delete)>(library, "wasmtime_error_delete");
                error_message = symbol<decltype(error_message)>(library,
                    "wasmtime_error_message");
                trap_delete = symbol<decltype(trap_delete)>(library, "wasm_trap_delete");
                valtype_new = symbol<decltype(valtype_new)>(library, "wasm_valtype_new");
                valtype_vec_new = symbol<decltype(valtype_vec_new)>(library,
                    "wasm_valtype_vec_new");
                functype_new = symbol<decltype(functype_new)>(library, "wasm_functype_new");
                functype_delete = symbol<decltype(functype_delete)>(library,
                    "wasm_functype_delete");
                byte_vec_delete = symbol<decltype(byte_vec_delete)>(library,
                    "wasm_byte_vec_delete");
                linker_new = symbol<decltype(linker_new)>(library, "wasmtime_linker_new");
                linker_define_func = symbol<decltype(linker_define_func)>(library,
                    "wasmtime_linker_define_func");
                linker_instantiate = symbol<decltype(linker_instantiate)>(library,
                    "wasmtime_linker_instantiate");
                linker_delete = symbol<decltype(linker_delete)>(library,
                    "wasmtime_linker_delete");
                caller_context = symbol<decltype(caller_context)>(library,
                    "wasmtime_caller_context");
                caller_export_get = symbol<decltype(caller_export_get)>(library,
                    "wasmtime_caller_export_get");
                memory_data = symbol<decltype(memory_data)>(library, "wasmtime_memory_data");
                memory_data_size = symbol<decltype(memory_data_size)>(library,
                    "wasmtime_memory_data_size");
                return engine_new && engine_delete && config_new && consume_fuel_set &&
                    engine_new_with_config && store_new && store_delete && store_context &&
                    module_new && module_delete && export_get && func_call &&
                    context_set_fuel && error_delete && error_message && trap_delete &&
                    valtype_new && valtype_vec_new && functype_new && functype_delete &&
                    byte_vec_delete && linker_new && linker_define_func &&
                    linker_instantiate && linker_delete && caller_context &&
                    caller_export_get && memory_data && memory_data_size;
            }
        };

        WasmApi& wasm() {
            static WasmApi api;
            return api;
        }

        // Stores the host currently owns. Every open is paired with a close, so a plugin that
        // cannot start and an unload both bring this back to where it was.
        std::size_t opened_stores = 0;

        wasmtime_store_t* open_store(wasm_engine_t* engine) {
            wasmtime_store_t* store = wasm().store_new(engine, nullptr, nullptr);
            if (store != nullptr) ++opened_stores;
            return store;
        }

        void close_store(wasmtime_store_t* store) {
            wasm().store_delete(store);
            --opened_stores;
        }

        // Fuel bounds the work of one call: a hook that never returns is stopped by the
        // runtime instead of holding the thread. Every call gets the same budget again, so a
        // plugin cannot save up work for a later call.
        void set_fuel(wasmtime_context_t* context, std::int32_t fuel) {
            if (wasmtime_error_t* failure = wasm().context_set_fuel(context,
                    static_cast<std::uint64_t>(fuel))) {
                wasm().error_delete(failure);
            }
        }

        // The host surface of ABI version 1. The import layer is the security boundary, so a
        // function is defined only when the plugin holds the capability behind it.
        constexpr const char* kHostModule = "terila";
        constexpr std::int32_t kMaxLogLine = 1024;
        constexpr std::int32_t kMaxChatLine = 256;

        // The guest's own window into its memory for this call, or nullptr when the range does
        // not fit or the module exports no memory. The guest is never handed a host pointer
        // and the host never aliases guest memory past the call.
        std::uint8_t* guest_buffer(wasmtime_caller_t* caller, std::int32_t pointer,
            std::int32_t length) {
            if (pointer < 0 || length < 0) return nullptr;
            wasmtime_extern_t item{};
            if (!wasm().caller_export_get(caller, "memory", 6, &item) ||
                item.kind != WASMTIME_EXTERN_MEMORY) {
                return nullptr;
            }
            wasmtime_context_t* context = wasm().caller_context(caller);
            const std::size_t start = static_cast<std::size_t>(pointer);
            const std::size_t size = wasm().memory_data_size(context, &item.of.memory);
            if (start + static_cast<std::size_t>(length) > size) return nullptr;
            return wasm().memory_data(context, &item.of.memory) + start;
        }

        // Reads a (pointer, length) pair out of the guest's memory and folds it to a single
        // line.
        bool read_guest_line(wasmtime_caller_t* caller, std::int32_t pointer, std::int32_t length,
            std::int32_t limit, std::string& out) {
            if (length <= 0 || length > limit) return false;
            const std::uint8_t* data = guest_buffer(caller, pointer, length);
            if (data == nullptr) return false;
            out.assign(reinterpret_cast<const char*>(data), static_cast<std::size_t>(length));
            // A line is what gets logged or shown: anything that could forge or split one is
            // replaced, and the padding at both ends is dropped.
            for (char& c : out) {
                if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
            }
            const auto first = out.find_first_not_of(' ');
            if (first == std::string::npos) {
                out.clear();
                return true;
            }
            out = out.substr(first, out.find_last_not_of(' ') - first + 1);
            return true;
        }

        // Every host function takes a (pointer, length) text argument after charging the
        // plugin's budget, and answers with the same documented values: 0 success, -1 bad
        // arguments, -2 over the events_per_second budget, -3 the host cannot serve it.
        // Errors are values, so a bad call is never a trap.
        std::int32_t read_argument(HostCall* call, wasmtime_caller_t* caller,
            const wasmtime_val_t* args, std::int32_t limit, std::string& line) {
            if (!call->take()) return -2;
            if (!read_guest_line(caller, args[0].of.i32, args[1].of.i32, limit, line)) return -1;
            return 0;
        }

        // terila.log_write(pointer, length) -> status
        wasm_trap_t* log_write(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args,
            std::size_t nargs, wasmtime_val_t* results, std::size_t nresults) {
            if (nargs != 2 || nresults != 1) return nullptr;
            auto* call = static_cast<HostCall*>(env);
            std::string line;
            results[0].kind = WASMTIME_I32;
            results[0].of.i32 = read_argument(call, caller, args, kMaxLogLine, line);
            if (results[0].of.i32 == 0 && !line.empty()) {
                LOG_INFO(Plugin, "[{}] {}", call->id, line);
            }
            return nullptr;
        }

        // terila.chat_send(pointer, length, target) -> status. Target 0 is every player and a
        // positive id is one player; the sink says whether anything was sent.
        wasm_trap_t* chat_send(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args,
            std::size_t nargs, wasmtime_val_t* results, std::size_t nresults) {
            if (nargs != 3 || nresults != 1) return nullptr;
            auto* call = static_cast<HostCall*>(env);
            std::string line;
            results[0].kind = WASMTIME_I32;
            results[0].of.i32 = read_argument(call, caller, args, kMaxChatLine, line);
            if (results[0].of.i32 != 0 || line.empty()) return nullptr;
            if (!chat_sink()) {
                results[0].of.i32 = -3;  // nothing is listening, e.g. before the world exists
                return nullptr;
            }
            if (!chat_sink()(args[2].of.i32, line)) {
                results[0].of.i32 = -1;  // no such player
            }
            return nullptr;
        }

        void put_i32(std::uint8_t* out, std::int32_t value) {
            const auto bits = static_cast<std::uint32_t>(value);
            for (int i = 0; i < 4; ++i) out[i] = static_cast<std::uint8_t>(bits >> (8 * i));
        }

        void put_f64(std::uint8_t* out, double value) {
            std::uint64_t bits = 0;
            std::memcpy(&bits, &value, sizeof bits);
            for (int i = 0; i < 8; ++i) out[i] = static_cast<std::uint8_t>(bits >> (8 * i));
        }

        // terila.player_count() -> the number of online players, or -2 over budget and -3 when
        // the host has no player source. Zero players is a valid answer, not an error.
        wasm_trap_t* player_count(void* env, wasmtime_caller_t*, const wasmtime_val_t*,
            std::size_t nargs, wasmtime_val_t* results, std::size_t nresults) {
            if (nargs != 0 || nresults != 1) return nullptr;
            auto* call = static_cast<HostCall*>(env);
            results[0].kind = WASMTIME_I32;
            if (!call->take()) {
                results[0].of.i32 = -2;
            } else if (!player_source().count) {
                results[0].of.i32 = -3;
            } else {
                results[0].of.i32 = static_cast<std::int32_t>(player_source().count());
            }
            return nullptr;
        }

        // terila.player_read(pointer, capacity, index) -> kPlayerRecordSize on success, or -1
        // for an index that is not online, -2 over budget, -3 no player source, -4 a capacity
        // that cannot hold one record, -5 a range outside the guest's memory.
        wasm_trap_t* player_read(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args,
            std::size_t nargs, wasmtime_val_t* results, std::size_t nresults) {
            if (nargs != 3 || nresults != 1) return nullptr;
            auto* call = static_cast<HostCall*>(env);
            results[0].kind = WASMTIME_I32;
            if (args[1].of.i32 < kPlayerRecordSize) {
                results[0].of.i32 = -4;
                return nullptr;
            }
            if (!call->take()) {
                results[0].of.i32 = -2;
                return nullptr;
            }
            if (!player_source().read) {
                results[0].of.i32 = -3;
                return nullptr;
            }
            std::uint8_t* out = guest_buffer(caller, args[0].of.i32, kPlayerRecordSize);
            if (out == nullptr) {
                results[0].of.i32 = -5;
                return nullptr;
            }
            PlayerRow row;
            if (args[2].of.i32 < 0 ||
                !player_source().read(static_cast<std::size_t>(args[2].of.i32), row)) {
                results[0].of.i32 = -1;
                return nullptr;
            }
            put_i32(out, row.entity);
            std::fill(out + 4, out + 4 + kPlayerNameSize, 0);
            const std::size_t name = std::min<std::size_t>(row.name.size(), kPlayerNameSize - 1);
            std::memcpy(out + 4, row.name.data(), name);
            put_i32(out + 36, row.dimension);
            put_f64(out + 40, row.x);
            put_f64(out + 48, row.y);
            put_f64(out + 56, row.z);
            results[0].of.i32 = kPlayerRecordSize;
            return nullptr;
        }

        // The only function shape the host serves: i32 parameters in, one i32 status out.
        wasm_functype_t* i32_function(std::size_t parameters) {
            std::vector<wasm_valtype_t*> params(parameters);
            for (auto*& type : params) type = wasm().valtype_new(WASM_I32);
            wasm_valtype_t* results[1] = { wasm().valtype_new(WASM_I32) };
            wasm_valtype_vec_t param_vec{};
            wasm_valtype_vec_t result_vec{};
            wasm().valtype_vec_new(&param_vec, params.size(), params.data());
            wasm().valtype_vec_new(&result_vec, 1, results);
            return wasm().functype_new(&param_vec, &result_vec);
        }

        using HostCallback = wasm_trap_t* (*)(void*, wasmtime_caller_t*, const wasmtime_val_t*,
            std::size_t, wasmtime_val_t*, std::size_t);

        // One host function on the linker. The environment is the plugin's own record, owned
        // by its slot, so the runtime is given nothing to free.
        bool define_function(wasmtime_linker_t* linker, const char* name,
            std::size_t parameters, HostCallback callback, HostCall* call, std::string& error) {
            wasm_functype_t* type = i32_function(parameters);
            if (type == nullptr) {
                error = std::string("the runtime refused the ") + name + " signature";
                return false;
            }
            const std::size_t length = std::char_traits<char>::length(name);
            wasmtime_error_t* failure = wasm().linker_define_func(linker, kHostModule,
                std::char_traits<char>::length(kHostModule), name, length, type, callback, call,
                nullptr);
            wasm().functype_delete(type);
            if (failure != nullptr) {
                wasm().error_delete(failure);
                error = std::string("the runtime refused to link ") + name;
                return false;
            }
            return true;
        }

        // Defines the host functions this plugin holds. A capability it was not granted is
        // simply not defined, so the module fails to instantiate instead of trapping later.
        bool define_host_functions(wasmtime_linker_t* linker, const Plugin& plugin,
            std::unique_ptr<HostCall>& call, std::string& error) {
            const auto holds = [&](std::string_view capability) {
                return std::find(plugin.effective.begin(), plugin.effective.end(), capability) !=
                    plugin.effective.end();
            };
            const bool logs = holds("log.write");
            const bool chats = holds("chat.send");
            const bool players = holds("player.read");
            if (!logs && !chats && !players) return true;

            call = std::make_unique<HostCall>();
            call->id = plugin.manifest.id;
            call->per_second = plugin.events_per_second;
            if (logs && !define_function(linker, "log_write", 2, &log_write, call.get(), error)) {
                return false;
            }
            if (chats && !define_function(linker, "chat_send", 3, &chat_send, call.get(), error)) {
                return false;
            }
            if (players &&
                (!define_function(linker, "player_count", 0, &player_count, call.get(), error) ||
                    !define_function(linker, "player_read", 3, &player_read, call.get(), error))) {
                return false;
            }
            return true;
        }

        // The runtime's own message names the import that could not be linked, which is what
        // an operator needs to fix the pack or the grant.
        std::string error_text(wasmtime_error_t* failure) {
            wasm_name_t text{};
            wasm().error_message(failure, &text);
            std::string message;
            if (text.data != nullptr && text.size > 0) {
                message.assign(text.data, text.size);
                wasm().byte_vec_delete(&text);
            }
            return message;
        }

        bool call_export(wasmtime_context_t* context, const wasmtime_instance_t& instance,
            const char* function, std::int32_t first, std::int32_t second,
            std::int32_t& result, std::string& error) {
            const WasmApi& api = wasm();
            wasmtime_extern_t item{};
            if (!api.export_get(context, &instance, function,
                    std::char_traits<char>::length(function), &item)) {
                error = std::string("no export named ") + function;
                return false;
            }
            if (item.kind != WASMTIME_EXTERN_FUNC) {
                error = std::string(function) + " is not a function";
                return false;
            }

            wasmtime_val_t args[2]{};
            args[0].kind = WASMTIME_I32;
            args[0].of.i32 = first;
            args[1].kind = WASMTIME_I32;
            args[1].of.i32 = second;
            wasmtime_val_t returned{};
            wasm_trap_t* trap = nullptr;
            wasmtime_error_t* failure = api.func_call(context, &item.of.func, args, 2, &returned,
                1, &trap);

            if (failure != nullptr) {
                error = "trapped";
                api.error_delete(failure);
                if (trap != nullptr) api.trap_delete(trap);
                return false;
            }
            if (trap != nullptr) {
                api.trap_delete(trap);
                error = "trapped";
                return false;
            }
            result = returned.of.i32;
            return true;
        }

#endif // TERILA_HAVE_WASMTIME

    } // namespace

    PluginHost& PluginHost::instance() {
        static PluginHost host;
        return host;
    }

    void PluginHost::set_chat_sink(std::function<bool(std::int32_t, std::string_view)> sink) {
        chat_sink() = std::move(sink);
    }

    void PluginHost::set_player_source(PlayerSource source) {
        player_source() = std::move(source);
    }

    void PluginHost::disable(std::string_view id, std::int32_t traps) {
#if defined(TERILA_HAVE_WASMTIME)
        const auto found = slots().find(std::string(id));
        if (found != slots().end()) {
            close_store(found->second.store);
            slots().erase(found);
        }
#endif
        for (Plugin& plugin : plugins_) {
            if (plugin.manifest.id != id) continue;
            plugin.enabled = false;
            plugin.status = "disabled: it trapped " + std::to_string(traps) + " times";
        }
        LOG_WARN(Plugin, "{} was disabled after {} traps", id, traps);
    }

    void PluginHost::unload_all() {
#if defined(TERILA_HAVE_WASMTIME)
        for (auto& entry : slots()) {
            close_store(entry.second.store);
        }
        slots().clear();
        if (engine_ != nullptr) {
            wasm().engine_delete(static_cast<wasm_engine_t*>(engine_));
            engine_ = nullptr;
        }
#endif
        plugins_.clear();
    }

    void PluginHost::load_all(const std::filesystem::path& packs,
        const std::filesystem::path& grants) {
        unload_all();

#if defined(TERILA_HAVE_WASMTIME)
        // Candidate list, in the same shape as the OpenCL loader: an environment override
        // first, then the names the runtime ships under on each platform.
        std::vector<std::string> candidates;
        if (const char* override_path = std::getenv("TERILA_WASM_RUNTIME")) {
            if (*override_path != '\0') candidates.emplace_back(override_path);
        }
#ifdef _WIN32
        candidates.insert(candidates.end(), { "wasmtime.dll", "wasmer.dll", "wasm3.dll" });
#else
        candidates.insert(candidates.end(), { "libwasmtime.so", "libwasmer.so", "libwasm3.so" });
#endif
        for (const std::string& candidate : candidates) {
#ifdef _WIN32
            void* library = static_cast<void*>(::LoadLibraryA(candidate.c_str()));
#else
            void* library = ::dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
            if (library == nullptr) continue;
            if (!wasm().resolve(library)) {
#ifdef _WIN32
                ::FreeLibrary(static_cast<HMODULE>(library));
#else
                ::dlclose(library);
#endif
                continue;
            }
            library_ = library;
            runtime_ = candidate;
            break;
        }

        if (library_ != nullptr) {
            wasm_config_t* config = wasm().config_new();
            wasm().consume_fuel_set(config, true);
            engine_ = wasm().engine_new_with_config(config);
            if (engine_ == nullptr) {
                runtime_.clear();
                library_ = nullptr;
            }
        }
#endif

        std::error_code ec;
        if (!std::filesystem::is_directory(packs, ec)) return;

        std::vector<std::filesystem::path> directories;
        for (const auto& entry : std::filesystem::directory_iterator(packs, ec)) {
            if (!entry.is_directory(ec)) continue;
            directories.push_back(entry.path());
        }
        std::sort(directories.begin(), directories.end());
        for (const auto& directory : directories) {
            load_one(directory, grants);
        }
    }

    void PluginHost::load_one(const std::filesystem::path& directory,
        const std::filesystem::path& grants) {
        std::error_code ec;
        const auto request_path = directory / kRequestFile;
        // Without a permission request the folder is not a plugin, so it is not listed at all.
        if (!std::filesystem::is_regular_file(request_path, ec)) return;

        Plugin plugin;
        plugin.directory = directory;

        // Every path below either enables the plugin or records one printable line saying why
        // it did not: a package is never silently dropped.
        const auto stop = [&](std::string status) {
            plugin.status = std::move(status);
            plugins_.push_back(std::move(plugin));
        };

        nlohmann::json manifest_json;
        std::string error;
        const auto manifest_path = directory / kManifestFile;
        if (!std::filesystem::is_regular_file(manifest_path, ec)) {
            stop("refused: plugin.json is missing");
            return;
        }
        if (!read_json(manifest_path, manifest_json, error)) {
            stop("refused: plugin.json " + error);
            return;
        }
        ManifestResult manifest = parse_manifest(manifest_json);
        if (!manifest) {
            stop(refused(*manifest.problem));
            return;
        }
        plugin.manifest = manifest.manifest;
        // The folder names the grant file and the log lines, so it must agree with the id.
        if (directory.filename().string() != plugin.manifest.id) {
            stop("refused: folder name does not match id '" + plugin.manifest.id + "'");
            return;
        }

        nlohmann::json request_json;
        if (!read_json(request_path, request_json, error)) {
            stop("refused: permissions.json " + error);
            return;
        }
        RequestResult request = parse_request(request_json);
        if (!request) {
            stop(refused(*request.problem));
            return;
        }
        if (request.id != plugin.manifest.id) {
            stop("refused: permissions.json id '" + request.id +
                "' does not match plugin.json id '" + plugin.manifest.id + "'");
            return;
        }
        plugin.requested = std::move(request.requested);

        // The grant is the operator's file, never the package's. A package cannot grant
        // itself anything.
        const auto grant_path = grants / (plugin.manifest.id + ".json");
        nlohmann::json grant_json;
        if (!std::filesystem::is_regular_file(grant_path, ec)) {
            plugin.accepted = true;
            stop("disabled: no grant file at " + grant_path.string());
            return;
        }
        if (!read_json(grant_path, grant_json, error)) {
            stop("disabled: grant " + error);
            return;
        }
        GrantResult grant = parse_grant(grant_json);
        if (!grant) {
            stop("disabled: grant " + grant.problem->text());
            return;
        }
        if (grant.id != plugin.manifest.id) {
            stop("disabled: the grant names '" + grant.id + "'");
            return;
        }
        plugin.granted = std::move(grant.granted);
        plugin.fuel_per_call = grant.limits.fuel_per_call;
        plugin.events_per_second = grant.limits.events_per_second;
        plugin.effective = effective_capabilities(plugin.requested, plugin.granted);
        plugin.accepted = true;
        if (plugin.effective.empty()) {
            stop("disabled: the grant allows none of the requested capabilities");
            return;
        }

#if defined(TERILA_HAVE_WASMTIME)
        if (library_ == nullptr || engine_ == nullptr) {
            stop("disabled: no WebAssembly runtime was found");
            return;
        }

        const auto module_path = directory / plugin.manifest.module;
        std::ifstream module_file(module_path, std::ios::binary);
        if (!module_file) {
            stop("refused: module '" + plugin.manifest.module + "' is missing");
            return;
        }
        const std::vector<char> bytes{ std::istreambuf_iterator<char>(module_file),
            std::istreambuf_iterator<char>() };

        wasmtime_module_t* module = nullptr;
        if (wasmtime_error_t* failure = wasm().module_new(static_cast<wasm_engine_t*>(engine_),
                reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &module)) {
            const std::string detail = error_text(failure);
            wasm().error_delete(failure);
            // The runtime names the offset and the opcode, which is what an author needs.
            stop(detail.empty() ? "refused: the module is not valid WebAssembly"
                                : "refused: the module is not valid WebAssembly: " + detail);
            return;
        }

        // The store is opened once and closed on every failure below, so a plugin that cannot
        // start leaves no store behind.
        wasmtime_store_t* store = open_store(static_cast<wasm_engine_t*>(engine_));
        wasmtime_context_t* context = wasm().store_context(store);
        Slot slot;
        slot.store = store;
        slot.fuel = plugin.fuel_per_call;

        wasmtime_linker_t* linker = wasm().linker_new(static_cast<wasm_engine_t*>(engine_));
        std::string link_error;
        if (linker == nullptr || !define_host_functions(linker, plugin, slot.call, link_error)) {
            if (linker != nullptr) wasm().linker_delete(linker);
            wasm().module_delete(module);
            close_store(store);
            stop("refused: " + (link_error.empty() ? std::string("no import namespace")
                                                  : link_error));
            return;
        }

        wasm_trap_t* trap = nullptr;
        wasmtime_error_t* failure = wasm().linker_instantiate(linker, context, module,
            &slot.instance, &trap);
        wasm().linker_delete(linker);
        wasm().module_delete(module);
        if (failure != nullptr) {
            const std::string message = error_text(failure);
            wasm().error_delete(failure);
            if (trap != nullptr) wasm().trap_delete(trap);
            close_store(store);
            // The message names the import that was not granted, or the module's own fault.
            stop("refused: " + (message.empty() ? std::string("the module could not be linked")
                                                : message));
            return;
        }
        if (trap != nullptr) {
            wasm().trap_delete(trap);
            close_store(store);
            stop("refused: instantiation trapped");
            return;
        }

        // The lifecycle hook is optional: a plugin without on_load is still a plugin. It runs
        // before the instance is registered, so a plugin that fails it never becomes callable
        // and holds no store.
        set_fuel(context, slot.fuel);
        std::int32_t result = 0;
        std::string hook_error;
        const bool hooked = call_export(context, slot.instance, "on_load", kApiVersion,
            static_cast<std::int32_t>(plugin.manifest.memory_pages), result, hook_error);
        if (!hooked && hook_error != "no export named on_load") {
            close_store(store);
            stop("refused: on_load " + hook_error);
            return;
        }

        slots()[plugin.manifest.id] = std::move(slot);
        plugin.enabled = true;
        plugin.status = "enabled";
        plugins_.push_back(std::move(plugin));
#else
        stop("disabled: this build has no WebAssembly runtime support");
#endif
    }

    std::size_t PluginHost::live_stores() const noexcept {
#if defined(TERILA_HAVE_WASMTIME)
        return opened_stores;
#else
        return 0;
#endif
    }

    bool PluginHost::call(std::string_view id, std::string_view function, std::int32_t first,
        std::int32_t second, std::int32_t& result, std::string& error) {
#if defined(TERILA_HAVE_WASMTIME)
        const auto found = slots().find(std::string(id));
        if (found == slots().end()) {
            error = "not loaded";
            return false;
        }
        Slot& slot = found->second;
        wasmtime_context_t* context = wasm().store_context(slot.store);
        set_fuel(context, slot.fuel);
        const bool ok = call_export(context, slot.instance, std::string(function).c_str(),
            first, second, result, error);
        // A trap drops the call and counts against the plugin; a plugin that keeps trapping is
        // released instead of being allowed to fail on every event for the rest of the run.
        if (!ok && error == "trapped" && ++slot.traps >= kMaxTraps) {
            disable(id, slot.traps);
        }
        return ok;
#else
        (void)id;
        (void)function;
        (void)first;
        (void)second;
        (void)result;
        error = "this build has no WebAssembly runtime support";
        return false;
#endif
    }

} // namespace terila::plugin
