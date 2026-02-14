#include "gc.h"
#include "core_types.h"
#include "runtime.h"

#ifdef __linux__
#include <malloc.h>  // for malloc_trim
#endif

void GarbageCollector::mark_ptr(void* ptr) {
    if (!ptr) return;
    marked_objects.insert(ptr);
}

// update tracking statistics when objects are allocated
// this is called from the track() template method in the header

bool GarbageCollector::is_marked(void* ptr) const {
    if (!ptr) return false;
    return marked_objects.count(ptr) > 0;
}

void GarbageCollector::clear_marks() {
    marked_objects.clear();
}

void GarbageCollector::mark_value(const Value& v) {
    if (v.is_array()) {
        mark_array(v.get_array());
    } else if (v.is_object()) {
        mark_object(v.get_object());
    } else if (v.is_handle()) {
        mark_handle(v.get_handle());
    }
}

void GarbageCollector::mark_array(const std::shared_ptr<std::vector<Value>>& arr) {
    if (!arr) return;

    void* ptr = static_cast<void*>(arr.get());
    if (is_marked(ptr)) return;

    mark_ptr(ptr);

    // mark all elements
    for (const auto& elem : *arr) {
        mark_value(elem);
    }
}

void GarbageCollector::mark_object(const std::shared_ptr<std::unordered_map<std::string, Value>>& obj) {
    if (!obj) return;

    void* ptr = static_cast<void*>(obj.get());
    if (is_marked(ptr)) return;

    mark_ptr(ptr);

    // mark all values
    for (const auto& [key, val] : *obj) {
        mark_value(val);
    }
}

void GarbageCollector::mark_handle(const std::shared_ptr<HandleData>& handle) {
    if (!handle) return;

    void* ptr = static_cast<void*>(handle.get());
    if (is_marked(ptr)) return;

    mark_ptr(ptr);

    // mark result and error values
    mark_value(handle->result);
    mark_value(handle->error);

    // mark task locals if present
    if (handle->task) {
        for (const auto& [key, val] : handle->task->locals) {
            mark_value(val);
        }

        for (const auto& scope : handle->task->block_scopes) {
            for (const auto& [key, val] : scope) {
                mark_value(val);
            }
        }

        mark_value(handle->task->result);
        mark_value(handle->task->error);
        mark_value(handle->task->flags.return_value);
        mark_value(handle->task->flags.throw_value);
    }
}

void GarbageCollector::prune_expired_locked() {
    auto it = tracked_objects.begin();
    while (it != tracked_objects.end()) {
        if (it->second.ptr.expired()) {
            it = tracked_objects.erase(it);
            refcount_freed++;
        } else {
            ++it;
        }
    }

    // shrink the tracking map if it's significantly oversized
    if (tracked_objects.bucket_count() > 4 * (tracked_objects.size() + 1)) {
        tracked_objects.rehash(tracked_objects.size());
    }

    // release freed pages back to the OS
#ifdef __linux__
    malloc_trim(0);
#endif
}

size_t GarbageCollector::sweep() {
    if (!enabled) return 0;

    std::lock_guard<std::mutex> lock(mutex);

    // identify unreachable objects that are still alive (part of cycles).
    // lock them temporarily so we can break their internal references.
    struct CycleMember {
        std::shared_ptr<void> locked_ptr;
        TrackedType type;
    };
    std::vector<CycleMember> cycle_members;

    size_t collected = 0;
    auto it = tracked_objects.begin();

    while (it != tracked_objects.end()) {
        void* ptr = it->first;

        // first check if the weak_ptr is still valid (object still exists)
        if (it->second.ptr.expired()) {
            // object was already freed by shared_ptr reference counting
            it = tracked_objects.erase(it);
            refcount_freed++;
            continue;
        }

        // check if this object is marked as reachable
        if (!is_marked(ptr)) {
            // object exists but is unreachable from roots as it's in a reference cycle.
            // lock the weak_ptr so we can break the cycle.
            auto locked = it->second.ptr.lock();
            if (locked) {
                cycle_members.push_back({std::move(locked), it->second.type});
            }
            it = tracked_objects.erase(it);
            collected++;
        } else {
            ++it;
        }
    }

    // break reference cycles by clearing the internal contents of unreachable objects.
    for (auto& member : cycle_members) {
        switch (member.type) {
            case TrackedType::Array: {
                auto arr = std::static_pointer_cast<std::vector<Value>>(member.locked_ptr);
                arr->clear();
                break;
            }
            case TrackedType::Object: {
                auto obj = std::static_pointer_cast<std::unordered_map<std::string, Value>>(member.locked_ptr);
                obj->clear();
                break;
            }
            case TrackedType::Handle: {
                auto handle = std::static_pointer_cast<HandleData>(member.locked_ptr);
                handle->result = Value::none();
                handle->error = Value::none();
                handle->task.reset();
                break;
            }
            default:
                break;
        }
    }

    // release temporary shared_ptrs
    cycle_members.clear();

    // shrink the mark set to release its peak-sized bucket array.
    std::unordered_set<void*>().swap(marked_objects);

    // shrink the tracking map if it's significantly smaller than its bucket count.
    // this frees the oversized hash table from peak allocation periods.
    if (tracked_objects.bucket_count() > 4 * (tracked_objects.size() + 1)) {
        tracked_objects.rehash(tracked_objects.size());
    }

    // on Linux, malloc doesn't return pages to the OS by default apparently?
#ifdef __linux__
    malloc_trim(0);
#endif

    return collected;
}

size_t GarbageCollector::collect(const std::vector<const Value*>& roots) {
    if (!enabled) return 0;

    total_collections++;

    // clear marks from previous collection
    clear_marks();

    // mark phase, start from all roots
    for (const Value* root : roots) {
        if (root) {
            mark_value(*root);
        }
    }

    // sweep phase, collect unmarked objects
    size_t collected = sweep();
    total_collected += collected;

    reset_allocation_count();

    return collected;
}

size_t GarbageCollector::force_collect(const std::vector<const Value*>& roots) {
    // Force collection regardless of threshold
    bool old_trigger = trigger_collection;
    trigger_collection = true;
    size_t result = collect(roots);
    trigger_collection = old_trigger;
    return result;
}

GarbageCollector::Stats GarbageCollector::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex);
    return Stats {
        tracked_objects.size(),
        allocation_count,
        total_collections,
        total_collected,
        total_allocated,
        peak_tracked,
        collection_threshold,
        refcount_freed,
        enabled
    };
}
