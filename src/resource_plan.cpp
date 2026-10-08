#include "resource_plan.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace olas {

namespace {

// Mirrors the ARCH_* constants in moonshine_engine.h.
constexpr int ARCH_SMALL_STREAMING = 4;

// Total cores the process may use, by machine size. An affinity budget, not
// a thread count: ONNX Runtime sizes its own pool from the cores it can see.
int total_affinity_cores(int logical_cores) {
    if (logical_cores <= 2)  return 1;
    if (logical_cores <= 4)  return 2;
    if (logical_cores <= 8)  return 3;
    return 4;
}

// Split the budget so the pools do not contend.
//   one model                  -> all of it
//   every model Small or less  -> one core each (Small runs well single
//                                 threaded; the pair stays light)
//   mixed                      -> one each, remainder to the heaviest arch
void split_cores(int total, const std::vector<int> &archs,
                 std::vector<int> &out) {
    const int n = (int)archs.size();
    out.assign((size_t)n, 0);
    if (n <= 0) return;

    if (n == 1) { out[0] = total; return; }

    bool all_light = true;
    for (int a : archs) if (a > ARCH_SMALL_STREAMING) all_light = false;

    if (all_light || total <= 1) {
        for (int i = 0; i < n; ++i) out[i] = 1;
        return;
    }

    int heaviest = 0;
    for (int i = 1; i < n; ++i)
        if (archs[i] > archs[heaviest]) heaviest = i;

    for (int i = 0; i < n; ++i) out[i] = 1;
    out[heaviest] += total - n;
}

int logical_cpu_count() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}

} // namespace

bool pin_current_thread(int first, int count) {
    if (count <= 0) return true;
    DWORD_PTR mask = 0;
    const int n = logical_cpu_count();
    for (int i = first; i < first + count && i < n; ++i)
        mask |= ((DWORD_PTR)1 << i);
    if (!mask) return false;
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
}

bool pin_thread(HANDLE thread, int first, int count) {
    if (count <= 0 || !thread) return true;
    DWORD_PTR mask = 0;
    const int n = logical_cpu_count();
    for (int i = first; i < first + count && i < n; ++i)
        mask |= ((DWORD_PTR)1 << i);
    if (!mask) return false;
    return SetThreadAffinityMask(thread, mask) != 0;
}

// Clear the affinity restriction on the calling thread only.
//
// This deliberately does NOT use SetProcessAffinityMask. That call rewrites
// the mask of every thread in the process, including the ONNX Runtime worker
// threads each transcriber just created, which collapses the per-model split
// back into one shared set and makes the two models contend. Linux has no
// equivalent problem because sched_setaffinity affects only the calling
// thread.
bool unpin_current_thread() {
    DWORD_PTR process_mask = 0, system_mask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask,
                                &system_mask))
        return false;
    return SetThreadAffinityMask(GetCurrentThread(), process_mask) != 0;
}

ResourcePlan plan_resources(int models,
                            const std::vector<int> &archs,
                            int force_threads,
                            int force_cores) {
    ResourcePlan p;
    p.models = std::max(1, models);

    const int logical = logical_cpu_count();
    p.force_single_thread = (force_threads >= 1);
    p.total_cores = total_affinity_cores(logical);

    // All-Small: keep the whole process to one core per model.
    bool all_light = !archs.empty();
    for (int a : archs) if (a > ARCH_SMALL_STREAMING) all_light = false;
    if (all_light && p.models > 1)
        p.total_cores = std::min(p.total_cores, p.models);

    if (force_cores == 0) {
        p.pin_affinity = false;
        p.total_cores = 0;
    } else if (force_cores > 0) {
        p.total_cores = std::min(force_cores, logical);
    }

    if (p.pin_affinity && p.total_cores > 0)
        split_cores(p.total_cores, archs, p.model_cores);

    std::ostringstream os;
    os << logical << " logical cores; " << p.models << " model(s); "
       << (p.force_single_thread ? "single-threaded (OLAS_THREADS)"
                                 : "ORT-managed threads");
    if (p.pin_affinity && !p.model_cores.empty()) {
        os << "; cores";
        for (size_t i = 0; i < p.model_cores.size(); ++i)
            os << (i ? "+" : " ") << p.model_cores[i];
    } else if (p.pin_affinity) {
        os << "; affinity " << p.total_cores << " core(s)";
    } else {
        os << "; affinity disabled";
    }
    p.rationale = os.str();

    return p;
}

} // namespace olas
