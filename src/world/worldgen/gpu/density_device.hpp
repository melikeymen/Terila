#pragma once

// The device half of the density field: an OpenCL loader, the kernel that walks the flattened
// graph, and the small engine that uploads a plan and evaluates a batch of grid points.
//
// OpenCL is opened through the system loader at runtime instead of a build dependency, so a
// machine without a usable device simply keeps the CPU path. A device must support doubles;
// the field is compared bit for bit against the CPU, and a device that computes it in singles
// would not match.

#include "world/worldgen/gpu/density_plan.hpp"

#include "log/logger.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace terila::world::worldgen::gpu {

    // The process-wide choice: terila.config decides it once at start, and a test flips it
    // between two runs to compare the paths in one process.
    enum class Backend : int {
        Cpu = 0,
        Gpu = 1,
        Auto = 2,
    };

    inline std::atomic<int>& backend_choice() noexcept {
        static std::atomic<int> choice{ static_cast<int>(Backend::Cpu) };
        return choice;
    }

    inline Backend backend() noexcept {
        return static_cast<Backend>(backend_choice().load(std::memory_order_relaxed));
    }

    inline void set_backend(Backend choice) noexcept {
        backend_choice().store(static_cast<int>(choice), std::memory_order_relaxed);
    }

    // Whether the backend wants the device at all. `Auto` resolves at the first batch: a
    // device that cannot run the kernel keeps the CPU silently.
    inline bool use_device() noexcept { return backend() != Backend::Cpu; }

    // The kernel mirrors the CPU evaluation node by node; the comment above each kind names
    // the branch of `DensityGraph::eval` it repeats.
    inline const char* density_kernel_source() {
        return R"CL(
#pragma OPENCL EXTENSION cl_khr_fp64 : enable

// The field is compared bit for bit with the CPU, so a multiply and the add that follows it
// must be two roundings on both sides: a fused multiply-add would round once.
#pragma OPENCL FP_CONTRACT OFF

#define NODE_INTS 15
#define NODE_DOUBLES 6
#define LAYER_DOUBLES 5

enum {
    K_CONSTANT = 0, K_Y, K_ZERO, K_ADD, K_MUL, K_MIN, K_MAX, K_SUB, K_DIV, K_NEGATE,
    K_ABS, K_SQUARE, K_CUBE, K_HALF_NEGATIVE, K_QUARTER_NEGATIVE, K_SQUEEZE, K_CLAMP,
    K_GRADIENT, K_NOISE, K_SHIFT, K_SHIFT_A, K_SHIFT_B, K_CACHE, K_SPLINE,
    K_RANGE_CHOICE, K_INTERVAL_SELECT, K_LERP, K_INTERPOLATED, K_BLEND_DENSITY,
    K_BLEND_ALPHA, K_BLEND_OFFSET, K_BEARDIFIER, K_FIND_TOP_SURFACE, K_OLD_BLENDED_NOISE,
    K_SLICE, K_DISTANCE_TO_POINT
};

__constant int GRAD_X[16] = { 1, -1, 1, -1, 1, -1, 1, -1, 0, 0, 0, 0, 1, 0, -1, 0 };
__constant int GRAD_Y[16] = { 1, 1, -1, -1, 0, 0, 0, 0, 1, -1, 1, -1, 1, -1, 1, -1 };
__constant int GRAD_Z[16] = { 0, 0, 0, 0, 1, 1, -1, -1, 1, 1, -1, -1, 0, 1, 0, -1 };

typedef struct {
    __global const int* nodes;
    __global const double* data;
    __global const int* children;
    __global const double* numbers;
    __global const double* points;
    __global const int* point_value;
    __global const double* layer_data;
    __global const float* layer_amplitude;
    __global const int* layer_perm;
    __global const uchar* perms;
    __global const int* noises;
    __global const int* blended_ints;
    __global const double* blended_doubles;
} Scene;

static inline double wrap_coord(double value) {
    const double band = 1.6777216e7;
    const double period = 3.3554432e7;
    if (value >= -band && value < band) {
        return value;
    }
    return value - floor(value / period + 0.5) * period;
}

static inline float smoothstep_of(float value) {
    return value * value * value * (value * (value * 6.0f - 15.0f) + 10.0f);
}

static inline float grad_dot(int hash, float x, float y, float z) {
    const int index = hash & 15;
    return (float)GRAD_X[index] * x + (float)GRAD_Y[index] * y + (float)GRAD_Z[index] * z;
}

static inline float lerp_of(float alpha, float first, float second) {
    return first + alpha * (second - first);
}

static inline float lattice_of(__global const uchar* perm, int fx, int fy, int fz, float rx,
    float ry, float rz, float ay) {
    const int x0 = perm[fx & 0xFF];
    const int x1 = perm[(fx + 1) & 0xFF];
    const int xy00 = perm[(x0 + fy) & 0xFF];
    const int xy01 = perm[(x0 + fy + 1) & 0xFF];
    const int xy10 = perm[(x1 + fy) & 0xFF];
    const int xy11 = perm[(x1 + fy + 1) & 0xFF];

    const float d000 = grad_dot(perm[(xy00 + fz) & 0xFF], rx, ry, rz);
    const float d100 = grad_dot(perm[(xy10 + fz) & 0xFF], rx - 1.0f, ry, rz);
    const float d010 = grad_dot(perm[(xy01 + fz) & 0xFF], rx, ry - 1.0f, rz);
    const float d110 = grad_dot(perm[(xy11 + fz) & 0xFF], rx - 1.0f, ry - 1.0f, rz);
    const float d001 = grad_dot(perm[(xy00 + fz + 1) & 0xFF], rx, ry, rz - 1.0f);
    const float d101 = grad_dot(perm[(xy10 + fz + 1) & 0xFF], rx - 1.0f, ry, rz - 1.0f);
    const float d011 = grad_dot(perm[(xy01 + fz + 1) & 0xFF], rx, ry - 1.0f, rz - 1.0f);
    const float d111 = grad_dot(perm[(xy11 + fz + 1) & 0xFF], rx - 1.0f, ry - 1.0f, rz - 1.0f);

    return lerp_of(smoothstep_of(rz),
        lerp_of(smoothstep_of(ay), lerp_of(smoothstep_of(rx), d000, d100),
            lerp_of(smoothstep_of(rx), d010, d110)),
        lerp_of(smoothstep_of(ay), lerp_of(smoothstep_of(rx), d001, d101),
            lerp_of(smoothstep_of(rx), d011, d111)));
}

static inline float perlin_get(__global const uchar* perm, double ox, double oy, double oz,
    double x_in, double y_in, double z_in) {
    const double x = wrap_coord(x_in) + ox;
    const double y = wrap_coord(y_in) + oy;
    const double z = wrap_coord(z_in) + oz;
    const int fx = (int)floor(x);
    const int fy = (int)floor(y);
    const int fz = (int)floor(z);
    const float rx = (float)(x - (double)fx);
    const float ry = (float)(y - (double)fy);
    const float rz = (float)(z - (double)fz);
    return lattice_of(perm, fx, fy, fz, rx, ry, rz, ry);
}

static inline float perlin_get_smeared(__global const uchar* perm, double ox, double oy,
    double oz, double x_in, double y_in, double z_in, double fudge_scale) {
    const double x = wrap_coord(x_in) + ox;
    const double y = wrap_coord(y_in) + oy;
    const double z = wrap_coord(z_in) + oz;
    const int fx = (int)floor(x);
    const int fy = (int)floor(y);
    const int fz = (int)floor(z);
    const float rx = (float)(x - (double)fx);
    const double ry_double = y - (double)fy;
    const float rz = (float)(z - (double)fz);

    const double limit = (y_in >= 0.0 && y_in < ry_double) ? y_in : ry_double;
    const double fudge = floor(limit / fudge_scale + 1.0e-7) * fudge_scale;
    const float fudged = (float)(ry_double - fudge);
    return lattice_of(perm, fx, fy, fz, rx, fudged, rz, (float)ry_double);
}

static inline float noise_sample(const Scene* scene, int index, double x, double y, double z) {
    const int first = scene->noises[index * 2];
    const int count = scene->noises[index * 2 + 1];
    float value = 0.0f;
    for (int i = 0; i < count; ++i) {
        const int layer = first + i;
        __global const double* data = scene->layer_data + layer * LAYER_DOUBLES;
        __global const uchar* perm = scene->perms + scene->layer_perm[layer];
        const float sampled = perlin_get(perm, data[0], data[1], data[2],
            x * data[3], y * data[3], z * data[3]);
        const float scaled = scene->layer_amplitude[layer] * sampled;
        value += scaled;
    }
    return value;
}

static inline float blended_stack(const Scene* scene, int first, int count, double x,
    double y, double z) {
    float total = 0.0f;
    for (int i = 0; i < count; ++i) {
        const int layer = first + i;
        __global const double* data = scene->layer_data + layer * LAYER_DOUBLES;
        __global const uchar* perm = scene->perms + scene->layer_perm[layer];
        const float sampled = perlin_get_smeared(perm, data[0], data[1], data[2],
            x * data[3], y * data[3], z * data[3], data[4]);
        total += sampled * scene->layer_amplitude[layer];
    }
    return total;
}

static inline double blended_sample(const Scene* scene, int index, double x, double y,
    double z) {
    __global const int* counts = scene->blended_ints + index * 6;
    __global const double* shape = scene->blended_doubles + index * 4;
    const double xz = shape[0];
    const double ym = shape[1];
    const double min_value = blended_stack(scene, counts[0], counts[1], x * xz, y * ym, z * xz);
    const double max_value = blended_stack(scene, counts[2], counts[3], x * xz, y * ym, z * xz);
    const double main_value = blended_stack(scene, counts[4], counts[5],
        x * xz / shape[2], y * ym / shape[3], z * xz / shape[2]);
    double t = main_value + 0.5;
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    return min_value + (max_value - min_value) * t;
}

#define FRAME_SLOTS 16
#define MAX_FRAMES 96

// One level of the walk. A node names its children by index, so the machine keeps a frame per
// open node instead of a call per level: a device stack is small, and a recursive walk of this
// tree runs out of it.
typedef struct {
    int node;
    int stage;
    double x;
    double y;
    double z;
    double v[FRAME_SLOTS];
} Frame;

#define RESTORE_COORDS() do { cx = frame->x; cy = frame->y; cz = frame->z; } while (0)

static double eval_root(const Scene* scene, int root, double px, double py, double pz) {
    Frame frames[MAX_FRAMES];
    int depth = 0;
    int current = root;
    double cx = px;
    double cy = py;
    double cz = pz;
    double pending = 0.0;
    int returning = 0;

    for (;;) {
        if (!returning) {
            if (current < 0) {
                pending = 0.0;
                returning = 1;
            }
            else {
                __global const int* node = scene->nodes + current * NODE_INTS;
                __global const double* data = scene->data + current * NODE_DOUBLES;
                const double v0 = data[0];
                const double v1 = data[1];
                const double v2 = data[2];
                const double v3 = data[3];
                int descend = 0;
                int child = -1;
                switch (node[0]) {
                case K_CONSTANT: pending = v0; returning = 1; break;
                case K_Y: pending = cy; returning = 1; break;
                case K_ZERO: pending = 0.0; returning = 1; break;
                case K_GRADIENT: {
                    const int axis = node[1];
                    const double coordinate = axis == 0 ? cx : (axis == 2 ? cz : cy);
                    const double span = v1 - v0;
                    double t = span == 0.0 ? 0.0 : (coordinate - v0) / span;
                    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
                    pending = v2 + (v3 - v2) * t;
                    returning = 1;
                    break;
                }
                case K_SHIFT: case K_SHIFT_A: case K_SHIFT_B: {
                    const int noise = node[5];
                    if (noise < 0) {
                        pending = 0.0;
                    }
                    else if (node[0] == K_SHIFT) {
                        pending = (double)noise_sample(scene, noise, cx * 0.25, cy * 0.25,
                            cz * 0.25) * 4.0;
                    }
                    else if (node[0] == K_SHIFT_A) {
                        pending = (double)noise_sample(scene, noise, cx * 0.25, 0.0,
                            cz * 0.25) * 4.0;
                    }
                    else {
                        pending = (double)noise_sample(scene, noise, cz * 0.25, cx * 0.25,
                            0.0) * 4.0;
                    }
                    returning = 1;
                    break;
                }
                case K_BLEND_ALPHA: pending = 1.0; returning = 1; break;
                case K_BLEND_OFFSET: case K_BEARDIFIER: pending = 0.0; returning = 1; break;
                case K_OLD_BLENDED_NOISE: {
                    const int blended = node[6];
                    pending = blended < 0 ? 0.0 : blended_sample(scene, blended, cx, cy, cz);
                    returning = 1;
                    break;
                }
                case K_DISTANCE_TO_POINT: {
                    const int first = node[11];
                    const int count = node[12];
                    const double ax = cx - (count > 0 ? scene->numbers[first] : 0.0);
                    const double ay = cy - (count > 1 ? scene->numbers[first + 1] : 0.0);
                    const double az = cz - (count > 2 ? scene->numbers[first + 2] : 0.0);
                    if (v0 != 0.0) {
                        const double mx = ax < 0.0 ? -ax : ax;
                        const double my = ay < 0.0 ? -ay : ay;
                        const double mz = az < 0.0 ? -az : az;
                        pending = mx + my + mz;
                    }
                    else {
                        pending = sqrt(ax * ax + ay * ay + az * az);
                    }
                    returning = 1;
                    break;
                }
                default: {
                    if (depth >= MAX_FRAMES) {
                        pending = 0.0;
                        returning = 1;
                        break;
                    }
                    Frame* frame = &frames[depth];
                    frame->node = current;
                    frame->stage = 0;
                    frame->x = cx;
                    frame->y = cy;
                    frame->z = cz;
                    if (node[0] == K_INTERPOLATED) {
                        const double cell_x = (double)node[7];
                        const double cell_y = (double)node[8];
                        const double x0 = floor(cx / cell_x) * cell_x;
                        const double y0 = floor(cy / cell_y) * cell_y;
                        const double z0 = floor(cz / cell_x) * cell_x;
                        if (x0 == cx && y0 == cy && z0 == cz) {
                            frame->stage = 100;
                        }
                        else {
                            frame->v[0] = x0;
                            frame->v[1] = y0;
                            frame->v[2] = z0;
                            frame->v[3] = cell_x;
                            frame->v[4] = cell_y;
                            cx = x0;
                            cy = y0;
                            cz = z0;
                        }
                    }
                    else if (node[0] == K_SLICE) {
                        const int axis = node[1];
                        if (axis == 0) cx = v0;
                        else if (axis == 1) cy = v0;
                        else cz = v0;
                    }
                    ++depth;
                    descend = 1;
                    child = node[2];
                    break;
                }
                }
                if (descend) {
                    current = child;
                    continue;
                }
            }
        }

        for (;;) {
            if (!returning) {
                break;
            }
            if (depth == 0) {
                return pending;
            }
            Frame* frame = &frames[depth - 1];
            __global const int* node = scene->nodes + frame->node * NODE_INTS;
            __global const double* data = scene->data + frame->node * NODE_DOUBLES;
            const double v0 = data[0];
            const double v1 = data[1];
            int descend = 0;
            int child = -1;

            switch (node[0]) {
            case K_ADD: case K_MUL: case K_MIN: case K_MAX: case K_SUB: case K_DIV:
                if (frame->stage == 0) {
                    frame->v[0] = pending;
                    frame->stage = 1;
                    descend = 1;
                    child = node[3];
                    break;
                }
                {
                    const double left = frame->v[0];
                    const double right = pending;
                    double value = 0.0;
                    if (node[0] == K_ADD) value = left + right;
                    else if (node[0] == K_MUL) value = left * right;
                    else if (node[0] == K_MIN) value = left < right ? left : right;
                    else if (node[0] == K_MAX) value = left > right ? left : right;
                    else if (node[0] == K_SUB) value = left - right;
                    else value = right == 0.0 ? 0.0 : left / right;
                    pending = value;
                    RESTORE_COORDS();
                    --depth;
                    continue;
                }
            case K_NOISE: {
                if (frame->stage == 0) {
                    frame->v[0] = pending;
                    frame->stage = 1;
                    descend = 1;
                    child = node[3];
                    break;
                }
                const int noise = node[5];
                if (noise < 0 || scene->noises[noise * 2 + 1] <= 0) {
                    pending = 0.0;
                }
                else {
                    pending = (double)noise_sample(scene, noise, frame->x * data[4] + frame->v[0],
                        frame->y * data[5], frame->z * data[4] + pending);
                }
                RESTORE_COORDS();
                --depth;
                continue;
            }
            case K_NEGATE: pending = -pending; RESTORE_COORDS(); --depth; continue;
            case K_ABS: pending = pending < 0.0 ? -pending : pending;
                RESTORE_COORDS(); --depth; continue;
            case K_SQUARE: pending = pending * pending; RESTORE_COORDS(); --depth; continue;
            case K_CUBE: pending = pending * pending * pending;
                RESTORE_COORDS(); --depth; continue;
            case K_HALF_NEGATIVE: pending = pending < 0.0 ? pending * 0.5 : pending;
                RESTORE_COORDS(); --depth; continue;
            case K_QUARTER_NEGATIVE: pending = pending < 0.0 ? pending * 0.25 : pending;
                RESTORE_COORDS(); --depth; continue;
            case K_SQUEEZE: {
                double clamped = pending;
                clamped = clamped < -1.0 ? -1.0 : (clamped > 1.0 ? 1.0 : clamped);
                pending = clamped / 2.0 - clamped * clamped * clamped / 24.0;
                RESTORE_COORDS();
                --depth;
                continue;
            }
            case K_CLAMP: {
                double value = pending;
                value = value < v0 ? v0 : value;
                pending = value > v1 ? v1 : value;
                RESTORE_COORDS();
                --depth;
                continue;
            }
            case K_CACHE: case K_BLEND_DENSITY: case K_SLICE:
                RESTORE_COORDS();
                --depth;
                continue;
            case K_INTERPOLATED: {
                if (frame->stage == 100) {
                    RESTORE_COORDS();
                    --depth;
                    continue;
                }
                frame->v[8 + frame->stage] = pending;
                ++frame->stage;
                if (frame->stage < 8) {
                    const int corner = frame->stage;
                    cx = frame->v[0] + ((corner & 1) != 0 ? frame->v[3] : 0.0);
                    cy = frame->v[1] + ((corner & 4) != 0 ? frame->v[4] : 0.0);
                    cz = frame->v[2] + ((corner & 2) != 0 ? frame->v[3] : 0.0);
                    descend = 1;
                    child = node[2];
                    break;
                }
                const double c000 = frame->v[8];
                const double c100 = frame->v[9];
                const double c010 = frame->v[10];
                const double c110 = frame->v[11];
                const double c001 = frame->v[12];
                const double c101 = frame->v[13];
                const double c011 = frame->v[14];
                const double c111 = frame->v[15];
                const double tx = (frame->x - frame->v[0]) / frame->v[3];
                const double ty = (frame->y - frame->v[1]) / frame->v[4];
                const double tz = (frame->z - frame->v[2]) / frame->v[3];
                const double bottom = (c000 + (c100 - c000) * tx) +
                    ((c010 + (c110 - c010) * tx) - (c000 + (c100 - c000) * tx)) * tz;
                const double top = (c001 + (c101 - c001) * tx) +
                    ((c011 + (c111 - c011) * tx) - (c001 + (c101 - c001) * tx)) * tz;
                pending = bottom + (top - bottom) * ty;
                RESTORE_COORDS();
                --depth;
                continue;
            }
            case K_SPLINE: {
                const int first = node[13];
                const int count = node[14];
                if (count <= 0) {
                    pending = 0.0;
                    RESTORE_COORDS();
                    --depth;
                    continue;
                }
                if (frame->stage == 0) {
                    const double input = pending;
                    const double first_location = scene->points[first * 2];
                    const double last_location = scene->points[(first + count - 1) * 2];
                    frame->v[0] = input;
                    if (input <= first_location) {
                        frame->v[1] = first_location;
                        frame->v[2] = scene->points[first * 2 + 1];
                        frame->stage = 10;
                        descend = 1;
                        child = scene->point_value[first];
                        break;
                    }
                    if (input >= last_location) {
                        const int last = first + count - 1;
                        frame->v[1] = last_location;
                        frame->v[2] = scene->points[last * 2 + 1];
                        frame->stage = 11;
                        descend = 1;
                        child = scene->point_value[last];
                        break;
                    }
                    int offset = 0;
                    while (offset + 1 < count &&
                        input >= scene->points[(first + offset + 1) * 2]) {
                        ++offset;
                    }
                    const double span = scene->points[(first + offset + 1) * 2] -
                        scene->points[(first + offset) * 2];
                    frame->v[2] = scene->points[(first + offset) * 2 + 1];
                    frame->v[3] = span;
                    frame->v[4] = (double)offset;
                    frame->v[5] = span <= 0.0 ? 0.0
                        : (input - scene->points[(first + offset) * 2]) / span;
                    frame->stage = 12;
                    descend = 1;
                    child = scene->point_value[first + offset];
                    break;
                }
                if (frame->stage == 10 || frame->stage == 11) {
                    pending = pending + frame->v[2] * (frame->v[0] - frame->v[1]);
                    RESTORE_COORDS();
                    --depth;
                    continue;
                }
                if (frame->stage == 12) {
                    frame->v[6] = pending;
                    frame->stage = 13;
                    descend = 1;
                    child = scene->point_value[first + (int)frame->v[4] + 1];
                    break;
                }
                {
                    const double y1 = frame->v[6];
                    const double y2 = pending;
                    if (frame->v[3] <= 0.0) {
                        pending = y1;
                        RESTORE_COORDS();
                        --depth;
                        continue;
                    }
                    const int upper = first + (int)frame->v[4] + 1;
                    const double t = frame->v[5];
                    const double a = frame->v[2] * frame->v[3] - (y2 - y1);
                    const double b = -scene->points[upper * 2 + 1] * frame->v[3] + (y2 - y1);
                    pending = (y1 + (y2 - y1) * t) + t * (1.0 - t) * (a + (b - a) * t);
                    RESTORE_COORDS();
                    --depth;
                    continue;
                }
            }
            case K_RANGE_CHOICE: {
                if (frame->stage == 0) {
                    const double value = pending;
                    frame->stage = 1;
                    descend = 1;
                    child = (value >= v0 && value < v1) ? node[3] : node[4];
                    break;
                }
                RESTORE_COORDS();
                --depth;
                continue;
            }
            case K_INTERVAL_SELECT: {
                if (frame->stage == 0) {
                    const double value = pending;
                    const int first_number = node[11];
                    const int numbers = node[12];
                    int offset = 0;
                    while (offset < numbers && value >= scene->numbers[first_number + offset]) {
                        ++offset;
                    }
                    if (offset >= node[10]) {
                        pending = 0.0;
                        RESTORE_COORDS();
                        --depth;
                        continue;
                    }
                    frame->stage = 1;
                    descend = 1;
                    child = scene->children[node[9] + offset];
                    break;
                }
                RESTORE_COORDS();
                --depth;
                continue;
            }
            case K_LERP: {
                if (frame->stage == 0) {
                    double alpha = pending;
                    alpha = alpha < 0.0 ? 0.0 : (alpha > 1.0 ? 1.0 : alpha);
                    frame->v[0] = alpha;
                    frame->stage = 1;
                    descend = 1;
                    child = node[3];
                    break;
                }
                if (frame->stage == 1) {
                    frame->v[1] = pending;
                    frame->stage = 2;
                    descend = 1;
                    child = node[4];
                    break;
                }
                pending = frame->v[1] + (pending - frame->v[1]) * frame->v[0];
                RESTORE_COORDS();
                --depth;
                continue;
            }
            default:
                pending = 0.0;
                RESTORE_COORDS();
                --depth;
                continue;
            }

            if (descend) {
                returning = 0;
                current = child;
                break;
            }
        }
    }
}

__kernel void density_field(__global const int* nodes, __global const double* data,
    __global const int* children, __global const double* numbers,
    __global const double* points, __global const int* point_value,
    __global const double* layer_data, __global const float* layer_amplitude,
    __global const int* layer_perm, __global const uchar* perms,
    __global const int* noises, __global const int* blended_ints,
    __global const double* blended_doubles, __global const double* coords,
    __global double* out, int count, int root) {
    const int id = (int)get_global_id(0);
    if (id >= count) {
        return;
    }
    Scene scene;
    scene.nodes = nodes;
    scene.data = data;
    scene.children = children;
    scene.numbers = numbers;
    scene.points = points;
    scene.point_value = point_value;
    scene.layer_data = layer_data;
    scene.layer_amplitude = layer_amplitude;
    scene.layer_perm = layer_perm;
    scene.perms = perms;
    scene.noises = noises;
    scene.blended_ints = blended_ints;
    scene.blended_doubles = blended_doubles;
    out[id] = eval_root(&scene, root, coords[id * 3], coords[id * 3 + 1],
        coords[id * 3 + 2]);
}
)CL";
    }

    // `auto` is a measurement rather than a guess: one chunk of field is sampled through both
    // paths once per process, and the device keeps the work only when it is the faster side.
    // The CPU side is handed in, because the tree walk lives with the generator.
    bool prefer_device_for(const DensityPlan& plan,
        const std::function<double(double, double, double)>& cpu_sample);

    namespace detail {

        using cl_int = std::int32_t;
        using cl_uint = std::uint32_t;
        using cl_bitfield = std::uint64_t;
        using cl_device_type = cl_bitfield;
        using cl_mem_flags = cl_bitfield;

        struct OpenClApi {
            void* library = nullptr;

            cl_int(*get_platform_ids)(cl_uint, void**, cl_uint*) = nullptr;
            cl_int(*get_device_ids)(void*, cl_device_type, cl_uint, void**, cl_uint*) = nullptr;
            cl_int(*get_device_info)(void*, cl_uint, std::size_t, void*, std::size_t*) = nullptr;
            void* (*create_context)(const std::intptr_t*, cl_uint, void**, void (*)(const char*,
                const void*, std::size_t, void*), void*, cl_int*) = nullptr;
            void* (*create_command_queue)(void*, void*, cl_bitfield, cl_int*) = nullptr;
            void* (*create_program_with_source)(void*, cl_uint, const char**,
                const std::size_t*, cl_int*) = nullptr;
            cl_int(*build_program)(void*, cl_uint, const void**, const char*,
                void (*)(void*, void*), void*) = nullptr;
            cl_int(*get_program_build_info)(void*, void*, cl_uint, std::size_t, void*,
                std::size_t*) = nullptr;
            void* (*create_kernel)(void*, const char*, cl_int*) = nullptr;
            cl_int(*set_kernel_arg)(void*, cl_uint, std::size_t, const void*) = nullptr;
            void* (*create_buffer)(void*, cl_mem_flags, std::size_t, void*, cl_int*) = nullptr;
            cl_int(*enqueue_write_buffer)(void*, void*, cl_uint, std::size_t, std::size_t,
                const void*, cl_uint, const void*, void**) = nullptr;
            cl_int(*enqueue_nd_range_kernel)(void*, void*, cl_uint, const std::size_t*,
                const std::size_t*, const std::size_t*, cl_uint, const void*,
                const void**) = nullptr;
            cl_int(*enqueue_read_buffer)(void*, void*, cl_uint, std::size_t, std::size_t,
                void*, cl_uint, const void*, void**) = nullptr;
            cl_int(*finish)(void*) = nullptr;
            cl_int(*release_mem_object)(void*) = nullptr;
            cl_int(*release_kernel)(void*) = nullptr;
            cl_int(*release_program)(void*) = nullptr;
            cl_int(*release_command_queue)(void*) = nullptr;
            cl_int(*release_context)(void*) = nullptr;

            bool open() {
                if (library != nullptr) {
                    return true;
                }
#if defined(_WIN32)
                library = static_cast<void*>(LoadLibraryA("OpenCL.dll"));
#else
                library = dlopen("libOpenCL.so.1", RTLD_NOW);
                if (library == nullptr) {
                    library = dlopen("libOpenCL.so", RTLD_NOW);
                }
#endif
                if (library == nullptr) {
                    return false;
                }
                return load_symbols();
            }

            ~OpenClApi() {
                if (library != nullptr) {
#if defined(_WIN32)
                    FreeLibrary(static_cast<HMODULE>(library));
#else
                    dlclose(library);
#endif
                }
            }

        private:
            template<typename T>
            bool symbol(T& out, const char* name) {
#if defined(_WIN32)
                out = reinterpret_cast<T>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
                out = reinterpret_cast<T>(dlsym(library, name));
#endif
                return out != nullptr;
            }

            bool load_symbols() {
                return symbol(get_platform_ids, "clGetPlatformIDs") &&
                    symbol(get_device_ids, "clGetDeviceIDs") &&
                    symbol(get_device_info, "clGetDeviceInfo") &&
                    symbol(create_context, "clCreateContext") &&
                    symbol(create_command_queue, "clCreateCommandQueue") &&
                    symbol(create_program_with_source, "clCreateProgramWithSource") &&
                    symbol(build_program, "clBuildProgram") &&
                    symbol(get_program_build_info, "clGetProgramBuildInfo") &&
                    symbol(create_kernel, "clCreateKernel") &&
                    symbol(set_kernel_arg, "clSetKernelArg") &&
                    symbol(create_buffer, "clCreateBuffer") &&
                    symbol(enqueue_write_buffer, "clEnqueueWriteBuffer") &&
                    symbol(enqueue_nd_range_kernel, "clEnqueueNDRangeKernel") &&
                    symbol(enqueue_read_buffer, "clEnqueueReadBuffer") &&
                    symbol(finish, "clFinish") &&
                    symbol(release_mem_object, "clReleaseMemObject") &&
                    symbol(release_kernel, "clReleaseKernel") &&
                    symbol(release_program, "clReleaseProgram") &&
                    symbol(release_command_queue, "clReleaseCommandQueue") &&
                    symbol(release_context, "clReleaseContext");
            }
        };

        inline constexpr cl_uint kDeviceName = 0x102B;
        inline constexpr cl_uint kDeviceExtensions = 0x1030;
        inline constexpr cl_uint kDeviceDoubleFpConfig = 0x1032;
        inline constexpr cl_uint kProgramBuildLog = 0x1183;
        inline constexpr cl_device_type kDeviceTypeGpu = 1ull << 2;
        inline constexpr cl_bitfield kMemReadOnly = 1ull << 2;
        inline constexpr cl_bitfield kMemCopyHostPtr = 1ull << 5;
        inline constexpr cl_bitfield kMemReadWrite = 1ull << 0;

        // Build options: doubles are required, and the kernel is compiled as OpenCL C 2.0
        // because the density tree is walked recursively.
        inline constexpr const char* kBuildOptions = "-cl-std=CL2.0";

    } // namespace detail

    // The batch the writer hands over: the grid points of one chunk.
    struct GridPoint {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

    class DensityDevice {
    public:
        static DensityDevice& shared() {
            // Never destroyed: a generator hands its tables back from the plan's destructor,
            // which can run during static teardown long after a plain static device would
            // have gone away. The loader and the device objects outlive everything instead.
            static DensityDevice* device = new DensityDevice();
            return *device;
        }

        // True once a device has been found. The first call opens the device; a second call
        // never retries, so a failed start does not log on every chunk.
        bool available() {
            std::lock_guard lock(mutex_);
            if (state_ == State::Unknown) {
                open();
            }
            return state_ == State::Ready;
        }

        const std::string& device_name() const noexcept { return device_name_; }
        const std::string& reason() const noexcept { return reason_; }

        // Evaluates `points` with the plan. The result is the same double the CPU evaluation
        // of the same tree returns, or false when the batch could not run.
        bool evaluate(const DensityPlan& plan, const std::vector<GridPoint>& points,
            std::vector<double>& out);

        // How many batches the device has answered. A caller that wants to know whether the
        // field really ran on the device compares this before and after a chunk.
        std::size_t batch_count() const noexcept { return batches_; }

        // Drops the buffers of a plan whose owner is gone. A generator that is destroyed must
        // not leave its tables on the device for the rest of the run.
        bool forget(std::uint64_t plan_id);

    private:
        enum class State {
            Unknown,
            Ready,
            Failed,
        };

        struct Upload {
            void* nodes = nullptr;
            void* data = nullptr;
            void* children = nullptr;
            void* numbers = nullptr;
            void* points = nullptr;
            void* point_value = nullptr;
            void* layer_data = nullptr;
            void* layer_amplitude = nullptr;
            void* layer_perm = nullptr;
            void* perms = nullptr;
            void* noises = nullptr;
            void* blended_ints = nullptr;
            void* blended_doubles = nullptr;
            void* coords = nullptr;
            void* out = nullptr;
            std::size_t coords_bytes = 0;
            std::size_t out_capacity = 0;
        };

        void open();
        bool upload(const DensityPlan& plan, Upload& target);
        bool bind_arguments(Upload& buffers);
        // A device that stops answering (a reset, an out of resources) is parked for the rest
        // of the run: retrying every chunk would stall the workers and fill the log.
        bool park(const std::string& why);

        detail::OpenClApi api_;
        State state_ = State::Unknown;
        std::mutex mutex_;
        std::string device_name_;
        std::string reason_;
        void* context_ = nullptr;
        void* queue_ = nullptr;
        void* program_ = nullptr;
        void* kernel_ = nullptr;
        std::unordered_map<std::uint64_t, Upload> uploads_;
        std::size_t batches_ = 0;
        bool parked_ = false;
        // The kernel holds the buffers of the plan that was bound last, and two dimensions
        // share one kernel, so the binding is tracked by plan and refreshed when it changes.
        std::uint64_t bound_plan_ = 0;
    };

} // namespace terila::world::worldgen::gpu
