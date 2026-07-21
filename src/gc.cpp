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
            est = 40 + ((ArrayObj *)p)->v.capacity() * VALUE_SIZE_ESTIMATE;
            break;
        case TrackedType::Object:
            est = 64 + ((ObjectObj *)p)->field_count() * VALUE_SIZE_ESTIMATE;
            break;
        case TrackedType::ClassInstance:
            est = 64 + ((ClassInstance *)p)->field_values.capacity() * VALUE_SIZE_ESTIMATE;
            break;
        case TrackedType::String:
            est = 32;
            break;
        default:
            est = 64;
            break;
    }

    // intrusive list push-front. No allocation, no hashing, cannot fail.
    p->gc_est = (est > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)est;
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

// iterative mark: an explicit worklist keeps C-stack usage independent of data shape
void GarbageCollector::mark_value(const Value &root) {
    auto push = [this](const Value &v) {
        if (v.is_heap()) {
            mark_stack.push_back(v.raw_bits());
        }
    };
    push(root);
    while (!mark_stack.empty()) {
        const Value v = Value::from_raw(mark_stack.back());
        mark_stack.pop_back();
        HeapHeader *p = v.heap_ptr();
        if (!p || p->gc_marked) {
            continue;
        }
        p->gc_marked = true;
        switch (p->type_tag) {
            case ValueTag::Array:
                for (const auto &elem : ((ArrayObj *)p)->v) {
                    push(elem);
                }
                break;
            case ValueTag::Object: {
                auto *obj = ((ObjectObj *)p);
                // shape-mode storage.
                for (const Value &f : obj->fields) {
                    push(f);
                }
                push(obj->lazy_payload);
                // dict-mode storage
                if (obj->dict_mode) {
                    for (const auto &name : obj->get_keys()) {
                        if (const Value *dv = obj->get_field(name)) {
                            push(*dv);
                        }
                    }
                }
                break;
            }
            case ValueTag::Handle: {
                auto *h = ((HandleData *)p);
                push(h->result);
                push(h->error);
                if (h->task) {
                    for (const auto &[k, tv] : h->task->locals) {
                        push(tv);
                    }
                    for (const auto &scope : h->task->block_scopes) {
                        for (const auto &[k, sv] : scope) {
                            push(sv);
                        }
                    }
                    push(h->task->result);
                    push(h->task->error);
                    push(h->task->flags.return_value);
                    push(h->task->flags.throw_value);
                }
                break;
            }
            case ValueTag::ClassInstance:
                for (const Value &f : ((ClassInstance *)p)->field_values) {
                    push(f);
                }
                break;
            case ValueTag::Function: {
                // only closures have captures (and only they are GC-tracked)
                auto *fd = ((FunctionData *)p);
                if (fd->captures) {
                    for (const auto &cell : *fd->captures) {
                        if (cell) {
                            push(*cell);
                        }
                    }
                }
                break;
            }
            case ValueTag::Delegate: {
                auto *d = ((DelegateData *)p);
                push(d->target);
                push(d->handler);
                break;
            }
            default:
                break; // string and others: leaf objects, no children
        }
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
                ((ArrayObj *)g)->v.clear();
                break;
            case ValueTag::Object:
                ((ObjectObj *)g)->clear_fields();
                ((ObjectObj *)g)->lazy_payload = Value::none();
                ((ObjectObj *)g)->lazy_captures.reset();
                break;
            case ValueTag::Handle: {
                auto *h = ((HandleData *)g);
                h->result = Value::none();
                h->error = Value::none();
                h->task.reset();
                break;
            }
            case ValueTag::ClassInstance:
                ((ClassInstance *)g)->field_values.clear();
                break;
            case ValueTag::Function:
                ((FunctionData *)g)->captures.reset();
                break;
            case ValueTag::Delegate: {
                auto *d = ((DelegateData *)g);
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
                delete ((ArrayObj *)g);
                break;
            case ValueTag::Object:
                delete ((ObjectObj *)g);
                break;
            case ValueTag::Handle:
                delete ((HandleData *)g);
                break;
            case ValueTag::ClassInstance:
                delete ((ClassInstance *)g);
                break;
            case ValueTag::Function:
                delete ((FunctionData *)g);
                break;
            case ValueTag::String:
                delete ((StringObj *)g);
                break;
            case ValueTag::Delegate:
                delete ((DelegateData *)g);
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
    // don't retain a pathologically grown worklist across collections (512KB cap)
    if (mark_stack.capacity() > 65536) {
        mark_stack.shrink_to_fit();
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
    return Stats{ tracked_count,   allocation_count, total_collections,    total_collected,
                  total_allocated, peak_tracked,     collection_threshold, enabled };
}
