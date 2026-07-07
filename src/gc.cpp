#include "gc.h"
#include "core_types.h"
#include "runtime.h"

#ifdef __linux__
#include <malloc.h>
#endif

// if the tracking map can't grow due to OOM, drop the entry rather than throw
void GarbageCollector::track(HeapHeader *p, TrackedType type) noexcept {
    if (!p || !enabled || p->gc_tracked) {
        return;
    }

    // NaN-boxed Value = 8 bytes
    constexpr size_t VALUE_SIZE_ESTIMATE = 8;
    size_t est = 0;
    switch (type) {
        case TrackedType::Array:
            est = 40 + static_cast<ArrayObj *>(p)->v.capacity() * VALUE_SIZE_ESTIMATE;
            break;
        case TrackedType::Object:
            est = 64 + static_cast<ObjectObj *>(p)->field_count() * VALUE_SIZE_ESTIMATE;
            break;
        case TrackedType::ClassInstance:
            est = 64 + static_cast<ClassInstance *>(p)->field_values.capacity() * VALUE_SIZE_ESTIMATE;
            break;
        case TrackedType::String:
            est = 32;
            break;
        default:
            est = 64;
            break;
    }

    // intrusive list push-front. No allocation, no hashing, cannot fail.
    p->gc_est = (est > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(est);
    p->gc_prev = nullptr;
    p->gc_next = gc_list_head;
    if (gc_list_head) {
        gc_list_head->gc_prev = p;
    }
    gc_list_head = p;
    p->gc_tracked = true;
    tracked_count++;

    allocation_count++;
    total_allocated++;
    estimated_memory_usage += est;

    if (tracked_count > peak_tracked) {
        peak_tracked = tracked_count;
    }
    if (memory_limit > 0 && estimated_memory_usage > memory_limit) {
        trigger_collection = true;
    }
    if (allocation_count >= collection_threshold) {
        trigger_collection = true;
    }
}

void GarbageCollector::untrack(HeapHeader *p) {
    if (!p || !p->gc_tracked) {
        return;
    }
    // Unlink from the intrusive list.
    if (p->gc_prev) {
        p->gc_prev->gc_next = p->gc_next;
    } else {
        gc_list_head = p->gc_next; // p was head
    }
    if (p->gc_next) {
        p->gc_next->gc_prev = p->gc_prev;
    }
    p->gc_next = p->gc_prev = nullptr;
    p->gc_tracked = false;
    size_t est = p->gc_est;
    estimated_memory_usage = (estimated_memory_usage >= est) ? estimated_memory_usage - est : 0;
    if (tracked_count > 0) {
        tracked_count--;
    }
}

// mark phase
void GarbageCollector::mark_ptr(HeapHeader *p) {
    if (!p) {
        return;
    }
    p->gc_marked = true;
}

bool GarbageCollector::is_marked(HeapHeader *p) const {
    return p && p->gc_marked;
}

void GarbageCollector::clear_marks() {
    for (HeapHeader *p = gc_list_head; p; p = p->gc_next) {
        p->gc_marked = false;
    }
}

void GarbageCollector::mark_value(const Value &v) {
    // Strings are leaf objects (no children) but are GC-tracked, so a reachable
    // one must be marked or the sweep would free it.
    if (v.is_string() && !v.is_sso()) {
        v.heap_ptr()->gc_marked = true;
        return;
    }
    if (v.is_array()) {
        mark_array(v.heap_ptr());
    } else if (v.is_object()) {
        mark_object(v.heap_ptr());
    } else if (v.is_handle()) {
        mark_handle(v.heap_ptr());
    } else if (v.is_class_instance()) {
        mark_class_instance(v.heap_ptr());
    } else if (v.is_function()) {
        // closures with captures are GC-tracked, use gc_marked to break cycles.
        // non-closure functions have null captures and are skipped.
        auto *fd = static_cast<FunctionData *>(v.heap_ptr());
        if (!fd || !fd->captures || fd->gc_marked) {
            return;
        }
        fd->gc_marked = true;
        for (const auto &cell : *fd->captures) {
            if (cell) {
                mark_value(*cell);
            }
        }
    } else if (v.is_delegate()) {
        // Delegate holds two Value children (target + handler); either may form a
        // cycle back to the delegate, so guard with the intrusive mark bit.
        HeapHeader *p = v.heap_ptr();
        if (p && !is_marked(p)) {
            mark_ptr(p);
            auto *d = static_cast<DelegateData *>(p);
            mark_value(d->target);
            mark_value(d->handler);
        }
    }
}

void GarbageCollector::mark_array(HeapHeader *p) {
    if (!p || is_marked(p)) {
        return;
    }
    mark_ptr(p);
    for (const auto &elem : static_cast<ArrayObj *>(p)->v) {
        mark_value(elem);
    }
}

void GarbageCollector::mark_object(HeapHeader *p) {
    if (!p || is_marked(p)) {
        return;
    }
    mark_ptr(p);
    auto *obj = static_cast<ObjectObj *>(p);
    // shape-mode storage.
    for (const Value &v : obj->fields) {
        mark_value(v);
    }
    // dict-mode storage
    if (obj->dict_mode) {
        for (const auto &name : obj->get_keys()) {
            if (const Value *v = obj->get_field(name)) {
                mark_value(*v);
            }
        }
    }
}

void GarbageCollector::mark_handle(HeapHeader *p) {
    if (!p || is_marked(p)) {
        return;
    }
    mark_ptr(p);
    auto *h = static_cast<HandleData *>(p);
    mark_value(h->result);
    mark_value(h->error);
    if (h->task) {
        for (const auto &[k, v] : h->task->locals) {
            mark_value(v);
        }
        for (const auto &scope : h->task->block_scopes) {
            for (const auto &[k, v] : scope) {
                mark_value(v);
            }
        }
        mark_value(h->task->result);
        mark_value(h->task->error);
        mark_value(h->task->flags.return_value);
        mark_value(h->task->flags.throw_value);
    }
}

void GarbageCollector::mark_class_instance(HeapHeader *p) {
    if (!p || is_marked(p)) {
        return;
    }
    mark_ptr(p);
    for (const Value &v : static_cast<ClassInstance *>(p)->field_values) {
        mark_value(v);
    }
}

// sweep phase: collect cycle garbage.
//  1. find unreachable tracked_objects, drop from registry
//  2. clear containers -> breaks cycles, may cascade-free children
//  3. delete what remains
size_t GarbageCollector::sweep() {
    if (!enabled) {
        return 0;
    }

    // walk the intrusive list, unlinking unreachable objects into `garbage`
    std::vector<HeapHeader *> garbage;
    HeapHeader *p = gc_list_head;
    while (p) {
        HeapHeader *next = p->gc_next;
        if (!p->gc_marked) {
            // unlink
            if (p->gc_prev) {
                p->gc_prev->gc_next = p->gc_next;
            } else {
                gc_list_head = p->gc_next;
            }
            if (p->gc_next) {
                p->gc_next->gc_prev = p->gc_prev;
            }
            p->gc_next = p->gc_prev = nullptr;
            p->gc_tracked = false;
            size_t est = p->gc_est;
            estimated_memory_usage = (estimated_memory_usage >= est) ? estimated_memory_usage - est : 0;
            if (tracked_count > 0) {
                tracked_count--;
            }
            garbage.push_back(p);
        }
        p = next;
    }

    // clear container contents (breaks cycles)
    for (HeapHeader *g : garbage) {
        switch (g->type_tag) {
            case ValueTag::Array:
                static_cast<ArrayObj *>(g)->v.clear();
                break;
            case ValueTag::Object:
                static_cast<ObjectObj *>(g)->clear_fields();
                break;
            case ValueTag::Handle: {
                auto *h = static_cast<HandleData *>(g);
                h->result = Value::none();
                h->error = Value::none();
                h->task.reset();
                break;
            }
            case ValueTag::ClassInstance:
                static_cast<ClassInstance *>(g)->field_values.clear();
                break;
            case ValueTag::Function:
                static_cast<FunctionData *>(g)->captures.reset();
                break;
            case ValueTag::Delegate: {
                auto *d = static_cast<DelegateData *>(g);
                d->target = Value::none();
                d->handler = Value::none();
                break;
            }
            default:
                break; // string and others: no children
        }
    }

    // delete
    for (HeapHeader *g : garbage) {
        switch (g->type_tag) {
            case ValueTag::Array:
                delete static_cast<ArrayObj *>(g);
                break;
            case ValueTag::Object:
                delete static_cast<ObjectObj *>(g);
                break;
            case ValueTag::Handle:
                delete static_cast<HandleData *>(g);
                break;
            case ValueTag::ClassInstance:
                delete static_cast<ClassInstance *>(g);
                break;
            case ValueTag::Function:
                delete static_cast<FunctionData *>(g);
                break;
            case ValueTag::String:
                delete static_cast<StringObj *>(g);
                break;
            case ValueTag::Delegate:
                delete static_cast<DelegateData *>(g);
                break;
            default:
                break;
        }
    }

    return garbage.size();
}

// full collection
size_t GarbageCollector::collect(const std::vector<const Value *> &roots) {
    if (!enabled) {
        return 0;
    }
    total_collections++;
    clear_marks();
    for (const Value *root : roots) {
        if (root) {
            mark_value(*root);
        }
    }
    size_t collected = sweep();
    total_collected += collected;
    reset_allocation_count();
    return collected;
}

size_t GarbageCollector::force_collect(const std::vector<const Value *> &roots) {
    bool old = trigger_collection;
    trigger_collection = true;
    size_t r = collect(roots);
    trigger_collection = old;
    return r;
}

// TODO?: consolidate these variables into the Stats struct so that we can just directly return that instead of copying
GarbageCollector::Stats GarbageCollector::get_stats() const {
    return Stats{
        tracked_count,
        allocation_count,
        total_collections,
        total_collected,
        total_allocated,
        peak_tracked,
        collection_threshold,
        enabled
    };
}
