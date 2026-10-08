#pragma once

#include <string>
#include <vector>

// Resource planning for OLAS on Windows. Mirrors the Linux resource_plan:
// the models are given separate core budgets so their ONNX Runtime pools do
// not contend, and single-threaded operation is the fallback for small
// machines.
//
// Measured on the Linux build (Ryzen 5600X, real speech):
//   MOONSHINE_ORT_SINGLE_THREAD set ->  1 thread, RTF 0.73
//   unset                           -> ORT-managed, RTF 0.42
//   Medium+Small sharing 4 cores    -> slowest model RTF ~0.95
//   Medium 3 cores + Small 1 core   -> slowest model RTF ~0.81
//
// The same reasoning applies here: the knob is a boolean, so the choice is
// set/unset, and affinity is what bounds the pool.

namespace olas {

struct ResourcePlan {
    int  models         = 1;
    int  total_cores    = 0;    // affinity budget for the whole process
    bool pin_affinity   = true;

    // False (default): leave MOONSHINE_ORT_SINGLE_THREAD unset so ORT sizes
    // its own pool; bound it with affinity. True: force one thread per model.
    bool force_single_thread = false;

    // Per-model core count. Empty means every model shares total_cores.
    std::vector<int> model_cores;

    // Human-readable summary for the log.
    std::string rationale;
};

// `archs` is the architecture number per model, used to give the heavier
// model the larger share. `force_threads` / `force_cores` come from
// OLAS_THREADS / OLAS_CPU_CORES, or -1 for auto.
ResourcePlan plan_resources(int models,
                            const std::vector<int> &archs,
                            int force_threads,
                            int force_cores);

// Restrict the calling thread to `count` logical CPUs starting at `first`.
// Used before each transcriber is built so its ORT session sizes its pool
// from that set. Returns false if Windows refused.
bool pin_current_thread(int first, int count);

// Restrict the whole process to `count` CPUs from 0. Called after all
// transcribers exist so capture and UI are not confined to one model's share.
bool pin_process(int count);

} // namespace olas
