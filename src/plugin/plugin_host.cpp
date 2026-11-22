#include "plugin/plugin_host.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <utility>

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

#if defined(TERILA_HAVE_WASMTIME)

        struct Slot {
            wasmtime_store_t* store = nullptr;
            wasmtime_instance_t* instance = nullptr;
        };

        // Loaded plugins own one store each, so a runaway plugin cannot disturb another one's
        // fuel or state.
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
            decltype(&::wasmtime_instance_new) instance_new = nullptr;
            decltype(&::wasmtime_instance_export_get) export_get = nullptr;
            decltype(&::wasmtime_func_call) func_call = nullptr;
            decltype(&::wasmtime_context_set_fuel) context_set_fuel = nullptr;
            decltype(&::wasmtime_error_delete) error_delete = nullptr;
            decltype(&::wasm_trap_delete) trap_delete = nullptr;

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
                instance_new = symbol<decltype(instance_new)>(library, "wasmtime_instance_new");
                export_get = symbol<decltype(export_get)>(library,
                    "wasmtime_instance_export_get");
                func_call = symbol<decltype(func_call)>(library, "wasmtime_func_call");
                context_set_fuel = symbol<decltype(context_set_fuel)>(library,
                    "wasmtime_context_set_fuel");
                error_delete = symbol<decltype(error_delete)>(library, "wasmtime_error_delete");
                trap_delete = symbol<decltype(trap_delete)>(library, "wasm_trap_delete");
                return engine_new && engine_delete && config_new && consume_fuel_set &&
                    engine_new_with_config && store_new && store_delete && store_context &&
                    module_new && module_delete && instance_new && export_get && func_call &&
                    context_set_fuel && error_delete && trap_delete;
            }
        };

        WasmApi& wasm() {
            static WasmApi api;
            return api;
        }

        // One call into a plugin. Fuel bounds the work: a hook that never returns is stopped
        // by the runtime instead of holding the thread.
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

    void PluginHost::unload_all() {
#if defined(TERILA_HAVE_WASMTIME)
        for (auto& [id, slot] : slots()) {
            (void)id;
            delete slot.instance;
            wasm().store_delete(slot.store);
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

        nlohmann::json manifest_json;
        std::string error;
        const auto manifest_path = directory / kManifestFile;
        if (!std::filesystem::is_regular_file(manifest_path, ec)) {
            plugin.status = "refused: plugin.json is missing";
            plugins_.push_back(std::move(plugin));
            return;
        }
        if (!read_json(manifest_path, manifest_json, error)) {
            plugin.status = "refused: plugin.json " + error;
            plugins_.push_back(std::move(plugin));
            return;
        }
        ManifestResult manifest = parse_manifest(manifest_json);
        if (!manifest) {
            plugin.status = refused(*manifest.problem);
            plugins_.push_back(std::move(plugin));
            return;
        }
        plugin.manifest = manifest.manifest;
        // The folder names the grant file and the log lines, so it must agree with the id.
        if (directory.filename().string() != plugin.manifest.id) {
            plugin.status = "refused: folder name does not match id '" +
                plugin.manifest.id + "'";
            plugins_.push_back(std::move(plugin));
            return;
        }

        nlohmann::json request_json;
        if (!read_json(request_path, request_json, error)) {
            plugin.status = "refused: permissions.json " + error;
            plugins_.push_back(std::move(plugin));
            return;
        }
        RequestResult request = parse_request(request_json);
        if (!request) {
            plugin.status = refused(*request.problem);
            plugins_.push_back(std::move(plugin));
            return;
        }
        if (request.id != plugin.manifest.id) {
            plugin.status = "refused: permissions.json id '" + request.id +
                "' does not match plugin.json id '" + plugin.manifest.id + "'";
            plugins_.push_back(std::move(plugin));
            return;
        }
        plugin.requested = std::move(request.requested);

        // The grant is the operator's file, never the package's. A package cannot grant
        // itself anything.
        const auto grant_path = grants / (plugin.manifest.id + ".json");
        nlohmann::json grant_json;
        if (!std::filesystem::is_regular_file(grant_path, ec)) {
            plugin.accepted = true;
            plugin.status = "disabled: no grant file at " + grant_path.string();
            plugins_.push_back(std::move(plugin));
            return;
        }
        if (!read_json(grant_path, grant_json, error)) {
            plugin.status = "disabled: grant " + error;
            plugins_.push_back(std::move(plugin));
            return;
        }
        GrantResult grant = parse_grant(grant_json);
        if (!grant) {
            plugin.status = "disabled: grant " + grant.problem->text();
            plugins_.push_back(std::move(plugin));
            return;
        }
        if (grant.id != plugin.manifest.id) {
            plugin.status = "disabled: the grant names '" + grant.id + "'";
            plugins_.push_back(std::move(plugin));
            return;
        }
        plugin.granted = std::move(grant.granted);
        plugin.effective = effective_capabilities(plugin.requested, plugin.granted);
        plugin.accepted = true;
        if (plugin.effective.empty()) {
            plugin.status = "disabled: the grant allows none of the requested capabilities";
            plugins_.push_back(std::move(plugin));
            return;
        }

#if defined(TERILA_HAVE_WASMTIME)
        if (library_ == nullptr || engine_ == nullptr) {
            plugin.status = "disabled: no WebAssembly runtime was found";
            plugins_.push_back(std::move(plugin));
            return;
        }

        const auto module_path = directory / plugin.manifest.module;
        std::ifstream module_file(module_path, std::ios::binary);
        if (!module_file) {
            plugin.status = "refused: module '" + plugin.manifest.module + "' is missing";
            plugins_.push_back(std::move(plugin));
            return;
        }
        const std::vector<char> bytes{ std::istreambuf_iterator<char>(module_file),
            std::istreambuf_iterator<char>() };

        wasmtime_module_t* module = nullptr;
        if (wasmtime_error_t* failure = wasm().module_new(static_cast<wasm_engine_t*>(engine_),
                reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &module)) {
            wasm().error_delete(failure);
            plugin.status = "refused: the module is not valid WebAssembly";
            plugins_.push_back(std::move(plugin));
            return;
        }

        Slot slot;
        slot.store = wasm().store_new(static_cast<wasm_engine_t*>(engine_), nullptr, nullptr);
        wasmtime_context_t* context = wasm().store_context(slot.store);
        wasmtime_instance_t instance{};
        wasm_trap_t* trap = nullptr;
        wasmtime_error_t* failure = wasm().instance_new(context, module, nullptr, 0, &instance,
            &trap);
        wasm().module_delete(module);
        if (failure != nullptr) {
            wasm().error_delete(failure);
            if (trap != nullptr) wasm().trap_delete(trap);
            wasm().store_delete(slot.store);
            // This version serves no host imports, so a module that wants any cannot start.
            plugin.status = "refused: it needs host imports this version does not serve";
            plugins_.push_back(std::move(plugin));
            return;
        }
        if (trap != nullptr) {
            wasm().trap_delete(trap);
            wasm().store_delete(slot.store);
            plugin.status = "refused: instantiation trapped";
            plugins_.push_back(std::move(plugin));
            return;
        }

        slot.instance = new wasmtime_instance_t(instance);
        slots()[plugin.manifest.id] = slot;

        // The lifecycle hook is optional: a plugin without on_load is still a plugin.
        std::int32_t result = 0;
        std::string hook_error;
        if (wasmtime_error_t* fuel = wasm().context_set_fuel(context, 10'000'000)) {
            wasm().error_delete(fuel);
        }
        const bool hooked = call_export(context, instance, "on_load", kApiVersion,
            static_cast<std::int32_t>(plugin.manifest.memory_pages), result, hook_error);
        if (!hooked && hook_error != "no export named on_load") {
            plugin.status = "refused: on_load " + hook_error;
            plugins_.push_back(std::move(plugin));
            return;
        }

        plugin.enabled = true;
        plugin.status = "enabled";
        plugins_.push_back(std::move(plugin));
#else
        plugin.status = "disabled: this build has no WebAssembly runtime support";
        plugins_.push_back(std::move(plugin));
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
        wasmtime_context_t* context = wasm().store_context(found->second.store);
        if (wasmtime_error_t* fuel = wasm().context_set_fuel(context, 10'000'000)) {
            wasm().error_delete(fuel);
        }
        return call_export(context, *found->second.instance, std::string(function).c_str(),
            first, second, result, error);
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
