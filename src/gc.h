#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

struct Value;
struct HandleData;
struct HeapHeader;

// Precise mark-and-sweep GC. It is the sole reclaimer of heap objects: every
// tracked object is freed by the sweep once it becomes unreachable from the
// roots. Run at allocator-paced safe-points during execution.
class GarbageCollector {
  public:
    static GarbageCollector &instance() {
        static GarbageCollector gc;
        return gc;
    }

    enum class TrackedType {
        Array,
        Object,
        Handle,
        ClassInstance,
        Function,
        String,
        Unknown
    };

    // Track a heap object so the mark-sweep collector can reclaim it.
    // noexcept since on OOM the entry is silently dropped
    void track(HeapHeader *p, TrackedType type) noexcept;

    // remove from tracking.
    void untrack(HeapHeader *p);

    // mark phase
    void mark_value(const Value &v);
    void mark_ptr(HeapHeader *p);
    bool is_marked(HeapHeader *p) const;
    void clear_marks();

    // collection
    size_t sweep();
    size_t collect(const std::vector<const Value *> &roots);
    size_t force_collect(const std::vector<const Value *> &roots);

    // statistics
    size_t get_tracked_count() const {
        return tracked_count;
    }
    size_t get_allocation_count() const {
        return allocation_count;
    }
    size_t get_total_collections() const {
        return total_collections;
    }
    size_t get_total_collected() const {
        return total_collected;
    }
    size_t get_total_allocated() const {
        return total_allocated;
    }
    size_t get_peak_tracked() const {
        return peak_tracked;
    }

    bool should_collect() const {
        return trigger_collection;
    }
    void reset_allocation_count() {
        allocation_count = 0;
        trigger_collection = false;
    }

    void set_collection_threshold(size_t t) {
        collection_threshold = t;
    }
    size_t get_collection_threshold() const {
        return collection_threshold;
    }

    void set_enabled(bool e) {
        enabled = e;
    }
    bool is_enabled() const {
        return enabled;
    }

    void set_memory_limit(size_t limit) {
        memory_limit = limit;
    }
    size_t get_memory_limit() const {
        return memory_limit;
    }
    size_t get_estimated_memory_usage() const {
        return estimated_memory_usage;
    }

    struct Stats {
        size_t tracked_count;
        size_t allocation_count;
        size_t total_collections;
        size_t total_collected;
        size_t total_allocated;
        size_t peak_tracked;
        size_t collection_threshold;
        bool enabled;
    };
    Stats get_stats() const;

  private:
    GarbageCollector() = default;
    ~GarbageCollector() = default;
    GarbageCollector(const GarbageCollector &) = delete;
    GarbageCollector &operator=(const GarbageCollector &) = delete;

    // intrusive doubly-linked list of all tracked heap objects
    HeapHeader *gc_list_head = nullptr;
    size_t tracked_count = 0;

    size_t allocation_count = 0;
    // allocations between collections, this gives roughly the best performance I saw, but ymmv?
    size_t collection_threshold = 4000;
    bool trigger_collection = false;
    bool enabled = true;

    size_t total_collections = 0;
    size_t total_collected = 0;
    size_t total_allocated = 0;
    size_t peak_tracked = 0;

    size_t memory_limit = 0;
    size_t estimated_memory_usage = 0;

    void mark_array(HeapHeader *p);
    void mark_object(HeapHeader *p);
    void mark_handle(HeapHeader *p);
    void mark_class_instance(HeapHeader *p);
};
