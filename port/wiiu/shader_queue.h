#pragma once
#include <algorithm>
#include <utility>

namespace gpu::compile_queue {
// Vertex misses force CPU interpretation. Pixel misses retain a GPU fallback.
// Cache warm-up is speculative until a draw explicitly demands that program.
template<class Job> unsigned priority(const Job &job) {
    return job.demanded ? (job.vs ? 0u : 1u) : 2u;
}
template<class Queue, class Job> void enqueue(Queue &queue, Job job) {
    const auto rank=priority(job);
    // Keep newest-demand-first within a class, as before, without allowing
    // a stream of pixel requests to starve a demanded vertex shader.
    auto at=std::find_if(queue.begin(),queue.end(),[rank](const auto &other){return priority(other)>=rank;});
    queue.insert(at,std::move(job));
}
}
