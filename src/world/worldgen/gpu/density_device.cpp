#include "world/worldgen/gpu/density_device.hpp"

#include <algorithm>
#include <chrono>
#include <vector>

namespace terila::world::worldgen::gpu {

    namespace {

        using detail::cl_bitfield;
        using detail::cl_int;
        using detail::cl_mem_flags;
        using detail::cl_uint;

        // A binding cannot be created from an empty host range, so the tables that are empty
        // for a given graph still get a buffer of one element.
        constexpr std::size_t kMinBufferBytes = 8;

        std::size_t bytes_of(std::size_t count, std::size_t element) {
            return std::max(kMinBufferBytes, count * element);
        }

    } // namespace

    void DensityDevice::open() {
        state_ = State::Failed;
        reason_.clear();

        if (!api_.open()) {
            reason_ = "the OpenCL loader is not installed";
            return;
        }

        cl_uint platform_count = 0;
        if (api_.get_platform_ids(0, nullptr, &platform_count) != 0 || platform_count == 0) {
            reason_ = "no OpenCL platform is present";
            return;
        }
        std::vector<void*> platforms(platform_count, nullptr);
        if (api_.get_platform_ids(platform_count, platforms.data(), nullptr) != 0) {
            reason_ = "the OpenCL platforms could not be listed";
            return;
        }

        for (void* platform : platforms) {
            cl_uint device_count = 0;
            if (api_.get_device_ids(platform, detail::kDeviceTypeGpu, 0, nullptr,
                    &device_count) != 0 ||
                device_count == 0) {
                continue;
            }
            std::vector<void*> devices(device_count, nullptr);
            if (api_.get_device_ids(platform, detail::kDeviceTypeGpu, device_count,
                    devices.data(), nullptr) != 0) {
                continue;
            }
            for (void* device : devices) {
                cl_bitfield double_config = 0;
                if (api_.get_device_info(device, detail::kDeviceDoubleFpConfig,
                        sizeof(double_config), &double_config, nullptr) != 0 ||
                    double_config == 0) {
                    reason_ = "the device cannot compute in doubles";
                    continue;
                }

                cl_int status = 0;
                void* context = api_.create_context(nullptr, 1, &device, nullptr, nullptr,
                    &status);
                if (context == nullptr || status != 0) {
                    reason_ = "the OpenCL context could not be created";
                    continue;
                }
                void* queue = api_.create_command_queue(context, device, 0, &status);
                if (queue == nullptr || status != 0) {
                    reason_ = "the OpenCL queue could not be created";
                    api_.release_context(context);
                    continue;
                }
                const char* source = density_kernel_source();
                void* program = api_.create_program_with_source(context, 1, &source, nullptr,
                    &status);
                if (program == nullptr || status != 0) {
                    reason_ = "the kernel source was not accepted";
                    api_.release_command_queue(queue);
                    api_.release_context(context);
                    continue;
                }

                const void* device_list[1] = { device };
                const cl_int built = api_.build_program(program, 1, device_list,
                    detail::kBuildOptions, nullptr, nullptr);
                if (built != 0) {
                    std::size_t log_size = 0;
                    if (api_.get_program_build_info(program, device, detail::kProgramBuildLog, 0,
                            nullptr, &log_size) == 0 &&
                        log_size > 1) {
                        std::string log(log_size, '\0');
                        api_.get_program_build_info(program, device, detail::kProgramBuildLog,
                            log_size, log.data(), nullptr);
                        while (!log.empty() && (log.back() == '\0' || log.back() == '\n')) {
                            log.pop_back();
                        }
                        reason_ = "the kernel did not build: " + log;
                        LOG_WARN(Chunk, "worldgen device kernel did not build: {}", log);
                    }
                    else {
                        reason_ = "the kernel did not build";
                        LOG_WARN(Chunk, "worldgen device kernel did not build (no log)");
                    }
                    api_.release_program(program);
                    api_.release_command_queue(queue);
                    api_.release_context(context);
                    continue;
                }

                void* kernel = api_.create_kernel(program, "density_field", &status);
                if (kernel == nullptr || status != 0) {
                    reason_ = "the kernel entry point is missing";
                    api_.release_program(program);
                    api_.release_command_queue(queue);
                    api_.release_context(context);
                    continue;
                }

                char name[256] = {};
                if (api_.get_device_info(device, detail::kDeviceName, sizeof(name), name,
                        nullptr) == 0) {
                    device_name_ = name;
                }
                else {
                    device_name_ = "an unnamed OpenCL device";
                }
                context_ = context;
                queue_ = queue;
                program_ = program;
                kernel_ = kernel;
                state_ = State::Ready;
                LOG_INFO(Chunk, "worldgen device: {} (OpenCL, doubles)", device_name_);
                return;
            }
        }

        if (reason_.empty()) {
            reason_ = "no OpenCL device could run the kernel";
        }
        // An operator who asked for the device by name wants to know why it is not used; the
        // automatic choice falls back quietly.
        if (backend() == Backend::Gpu) {
            LOG_WARN(Chunk, "worldgen backend 'gpu' unavailable: {}; the chunk workers keep "
                "the field", reason_);
        }
        else {
            LOG_INFO(Chunk, "worldgen backend 'auto': {}; the chunk workers keep the field",
                reason_);
        }
    }

    bool DensityDevice::upload(const DensityPlan& plan, Upload& target) {
        // An empty table has no host range to copy from, and a null host pointer makes the
        // whole upload fail, so those buffers are created from a zeroed scratch value.
        static const unsigned char empty_table[8] = {};
        const auto make = [this](const void* data, std::size_t count, std::size_t element) {
            const std::size_t bytes = bytes_of(count, element);
            const void* host = count == 0 ? static_cast<const void*>(empty_table) : data;
            cl_int status = 0;
            void* memory = api_.create_buffer(context_,
                detail::kMemReadOnly | detail::kMemCopyHostPtr, bytes,
                const_cast<void*>(host), &status);
            return status == 0 ? memory : nullptr;
        };

        const auto release = [this](Upload& buffers) {
            for (void* memory : { buffers.nodes, buffers.data, buffers.children, buffers.numbers,
                     buffers.points, buffers.point_value, buffers.layer_data,
                     buffers.layer_amplitude, buffers.layer_perm, buffers.perms, buffers.noises,
                     buffers.blended_ints, buffers.blended_doubles, buffers.coords,
                     buffers.out }) {
                if (memory != nullptr) {
                    api_.release_mem_object(memory);
                }
            }
            buffers = Upload{};
        };

        // The tables travel as flat integers and doubles: the kernel does the indexing, so
        // there is no struct layout to agree on between the two sides.
        std::vector<cl_int> node_ints;
        std::vector<double> node_doubles;
        node_ints.reserve(plan.nodes.size() * 15);
        node_doubles.reserve(plan.nodes.size() * 6);
        for (const auto& node : plan.nodes) {
            node_ints.insert(node_ints.end(), { static_cast<cl_int>(node.kind), node.axis,
                node.a, node.b, node.c, node.noise, node.blended, node.cell_xz,
                node.cell_height, node.first_child, node.child_count, node.first_number,
                node.number_count, node.first_point, node.point_count });
            node_doubles.insert(node_doubles.end(), { node.v0, node.v1, node.v2, node.v3,
                node.xz_scale, node.y_scale });
        }
        std::vector<double> point_data;
        std::vector<cl_int> point_values;
        point_data.reserve(plan.points.size() * 2);
        point_values.reserve(plan.points.size());
        for (const auto& point : plan.points) {
            point_data.push_back(point.location);
            point_data.push_back(point.derivative);
            point_values.push_back(point.value);
        }
        std::vector<double> layer_data;
        std::vector<float> layer_amplitude;
        std::vector<cl_int> layer_perm;
        layer_data.reserve(plan.layers.size() * 5);
        for (const auto& layer : plan.layers) {
            layer_data.insert(layer_data.end(), { layer.offset_x, layer.offset_y, layer.offset_z,
                layer.frequency, layer.smear });
            layer_amplitude.push_back(layer.amplitude);
            layer_perm.push_back(static_cast<cl_int>(layer.first_perm));
        }
        std::vector<cl_int> noises;
        for (const auto& noise : plan.noises) {
            noises.push_back(noise.first_layer);
            noises.push_back(noise.layer_count);
        }
        std::vector<cl_int> blended_ints;
        std::vector<double> blended_doubles;
        for (const auto& blended : plan.blended) {
            blended_ints.insert(blended_ints.end(), { blended.first_min, blended.count_min,
                blended.first_max, blended.count_max, blended.first_main, blended.count_main });
            blended_doubles.insert(blended_doubles.end(), { blended.xz_multiplier,
                blended.y_multiplier, blended.xz_factor, blended.y_factor });
        }

        target.nodes = make(node_ints.data(), node_ints.size(), sizeof(cl_int));
        target.data = make(node_doubles.data(), node_doubles.size(), sizeof(double));
        target.children = make(plan.children.data(), plan.children.size(), sizeof(cl_int));
        target.numbers = make(plan.numbers.data(), plan.numbers.size(), sizeof(double));
        target.points = make(point_data.data(), point_data.size(), sizeof(double));
        target.point_value = make(point_values.data(), point_values.size(), sizeof(cl_int));
        target.layer_data = make(layer_data.data(), layer_data.size(), sizeof(double));
        target.layer_amplitude = make(layer_amplitude.data(), layer_amplitude.size(),
            sizeof(float));
        target.layer_perm = make(layer_perm.data(), layer_perm.size(), sizeof(cl_int));
        target.perms = make(plan.permutations.data(), plan.permutations.size(), 1);
        target.noises = make(noises.data(), noises.size(), sizeof(cl_int));
        target.blended_ints = make(blended_ints.data(), blended_ints.size(), sizeof(cl_int));
        target.blended_doubles = make(blended_doubles.data(), blended_doubles.size(),
            sizeof(double));

        if (target.nodes == nullptr || target.data == nullptr || target.children == nullptr ||
            target.numbers == nullptr || target.points == nullptr ||
            target.point_value == nullptr || target.layer_data == nullptr ||
            target.layer_amplitude == nullptr || target.layer_perm == nullptr ||
            target.perms == nullptr || target.noises == nullptr ||
            target.blended_ints == nullptr || target.blended_doubles == nullptr) {
            release(target);
            LOG_WARN(Chunk, "worldgen device could not hold the density graph");
            return false;
        }
        return true;
    }

    bool DensityDevice::forget(std::uint64_t plan_id) {
        std::lock_guard lock(mutex_);
        if (state_ != State::Ready) {
            return false;
        }
        const auto found = uploads_.find(plan_id);
        if (found == uploads_.end()) {
            return false;
        }
        Upload& buffers = found->second;
        for (void* memory : { buffers.nodes, buffers.data, buffers.children, buffers.numbers,
                 buffers.points, buffers.point_value, buffers.layer_data,
                 buffers.layer_amplitude, buffers.layer_perm, buffers.perms, buffers.noises,
                 buffers.blended_ints, buffers.blended_doubles, buffers.coords, buffers.out }) {
            if (memory != nullptr) {
                api_.release_mem_object(memory);
            }
        }
        uploads_.erase(found);
        return true;
    }

    bool DensityDevice::park(const std::string& why) {
        parked_ = true;
        LOG_WARN(Chunk, "worldgen device stopped answering ({}); the chunk workers keep the "
            "field for the rest of the run", why);
        return false;
    }

    bool DensityDevice::bind_arguments(Upload& buffers) {
        void* arguments[15] = { buffers.nodes, buffers.data, buffers.children, buffers.numbers,
            buffers.points, buffers.point_value, buffers.layer_data, buffers.layer_amplitude,
            buffers.layer_perm, buffers.perms, buffers.noises, buffers.blended_ints,
            buffers.blended_doubles, buffers.coords, buffers.out };
        for (cl_uint index = 0; index < 15; ++index) {
            if (api_.set_kernel_arg(kernel_, index, sizeof(void*), &arguments[index]) != 0) {
                return false;
            }
        }
        return true;
    }

    bool DensityDevice::evaluate(const DensityPlan& plan, const std::vector<GridPoint>& points,
        std::vector<double>& out) {
        if (!available() || !plan.usable() || points.empty()) {
            return false;
        }

        // One queue, one kernel: two workers would otherwise interleave their buffers. The
        // device is a single resource, so the lock is held for the whole batch.
        std::lock_guard lock(mutex_);
        if (state_ != State::Ready || parked_) {
            return false;
        }

        auto found = uploads_.find(plan.id);
        if (found == uploads_.end()) {
            Upload fresh;
            if (!upload(plan, fresh)) {
                return false;
            }
            found = uploads_.emplace(plan.id, fresh).first;
        }
        Upload& buffers = found->second;

        const std::size_t count = points.size();
        const std::size_t coords_bytes = count * 3 * sizeof(double);
        if (buffers.coords_bytes < coords_bytes) {
            if (buffers.coords != nullptr) {
                api_.release_mem_object(buffers.coords);
            }
            buffers.coords = api_.create_buffer(context_, detail::kMemReadOnly, coords_bytes,
                nullptr, nullptr);
            buffers.coords_bytes = coords_bytes;
            bound_plan_ = 0;
        }
        if (buffers.out_capacity < count) {
            if (buffers.out != nullptr) {
                api_.release_mem_object(buffers.out);
            }
            buffers.out = api_.create_buffer(context_, detail::kMemReadWrite,
                count * sizeof(double), nullptr, nullptr);
            buffers.out_capacity = count;
            bound_plan_ = 0;
        }
        if (buffers.coords == nullptr || buffers.out == nullptr) {
            return park("the grid buffers could not be created");
        }
        if (bound_plan_ != plan.id) {
            if (!bind_arguments(buffers)) {
                return park("the kernel arguments were rejected");
            }
            bound_plan_ = plan.id;
        }

        std::vector<double> flat(count * 3);
        for (std::size_t i = 0; i < count; ++i) {
            flat[i * 3] = points[i].x;
            flat[i * 3 + 1] = points[i].y;
            flat[i * 3 + 2] = points[i].z;
        }
        const cl_int written = api_.enqueue_write_buffer(queue_, buffers.coords, 1, 0,
            coords_bytes, flat.data(), 0, nullptr, nullptr);
        if (written != 0) {
            return park("the grid points were rejected");
        }

        // The count and the root are the only arguments that change per batch; the buffers
        // were bound once when the plan was uploaded.
        const cl_int items = static_cast<cl_int>(count);
        const cl_int root = plan.root;
        if (api_.set_kernel_arg(kernel_, 15, sizeof(items), &items) != 0 ||
            api_.set_kernel_arg(kernel_, 16, sizeof(root), &root) != 0) {
            return park("the batch size was rejected");
        }

        const std::size_t global[1] = { count };
        const cl_int launched = api_.enqueue_nd_range_kernel(queue_, kernel_, 1, nullptr, global,
            nullptr, 0, nullptr, nullptr);
        if (launched != 0) {
            return park("the grid batch was refused");
        }
        out.assign(count, 0.0);
        const cl_int read = api_.enqueue_read_buffer(queue_, buffers.out, 1, 0,
            count * sizeof(double), out.data(), 0, nullptr, nullptr);
        const cl_int finished = api_.finish(queue_);
        if (read != 0 || finished != 0) {
            return park("the grid was not returned");
        }
        ++batches_;
        return true;
    }

} // namespace terila::world::worldgen::gpu

namespace terila::world::worldgen::gpu {

    bool prefer_device_for(const DensityPlan& plan,
        const std::function<double(double, double, double)>& cpu_sample) {
        static std::mutex mutex;
        static int decision = 0;

        std::lock_guard lock(mutex);
        if (decision != 0) {
            return decision == 1;
        }
        DensityDevice& device = DensityDevice::shared();
        if (!device.available() || !plan.usable()) {
            decision = 2;
            return false;
        }

        std::vector<GridPoint> points;
        points.reserve(600);
        for (int iy = 0; iy < 24; ++iy) {
            for (int iz = 0; iz < 5; ++iz) {
                for (int ix = 0; ix < 5; ++ix) {
                    points.push_back(GridPoint{ static_cast<double>(ix * 4),
                        static_cast<double>(-64 + iy * 8), static_cast<double>(iz * 4) });
                }
            }
        }

        std::vector<double> values;
        // The first dispatch pays for the upload, so it is not part of the measurement.
        if (!device.evaluate(plan, points, values)) {
            decision = 2;
            return false;
        }
        const auto device_start = std::chrono::steady_clock::now();
        for (int round = 0; round < 4; ++round) {
            if (!device.evaluate(plan, points, values)) {
                decision = 2;
                return false;
            }
        }
        const double device_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - device_start).count() / 4.0;

        const auto cpu_start = std::chrono::steady_clock::now();
        double checksum = 0.0;
        for (const auto& point : points) {
            checksum += cpu_sample(point.x, point.y, point.z);
        }
        const double cpu_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - cpu_start).count();

        // A device that answers the same field but draws the same power for it is not worth
        // the transfer, and `checksum` keeps the compiler from dropping the CPU pass.
        decision = (device_ms < cpu_ms && checksum != 1.23456789e300) ? 1 : 2;
        LOG_INFO(Chunk, "worldgen backend 'auto': the device takes {:.2f} ms for one field, the "
            "chunk workers {:.2f} ms; using {}", device_ms, cpu_ms,
            decision == 1 ? "the device" : "the chunk workers");
        return decision == 1;
    }

} // namespace terila::world::worldgen::gpu
