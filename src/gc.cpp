#include "gc.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "core_types.h"
#include "runtime.h"

#ifdef __linux__
#include <malloc.h>
#endif

GarbageCollector GarbageCollector::singleton;

// if the tracking map can't grow due to OOM, drop the entry rather than throw
void GarbageCollector::track(HeapHeader *p, TrackedType type) noexcept {
    if (!p || !enabled || p->gc_tracked) {
        return;
    }

    // NaN-boxed Value = 8 bytes
    constexpr size_t VALUE_SIZE_ESTIMATE = 8;
    // header sizes come from sizeof, so the estimate tracks the structs instead of differing from constants.
    size_t est = 0;
    switch (type) {
        case TrackedType::Array:
            est = sizeof(ArrayObj) + ((ArrayObj *)p)->v.capacity() * VALUE_SIZE_ESTIMATE;
            break;
        case TrackedType::Object:
            est = sizeof(ObjectObj) + ((ObjectObj *)p)->field_count() * VALUE_SIZE_ESTIMATE;
            break;
        case TrackedType::ClassInstance:
            est = sizeof(ClassInstance) + ((ClassInstance *)p)->field_values.capacity() * VALUE_SIZE_ESTIMATE;
            break;
        case TrackedType::String:
            est = sizeof(StringObj);
            break;
        case TrackedType::Function:
            est = sizeof(FunctionData);
            break;
        default:
            est = 64;
            break;
    }

    p->gc_est = (est > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)est;
    try {
        p->gc_index = tracked_objects.size();
        tracked_objects.push_back(p);
    } catch (...) {
        return;
    }
    p->gc_tracked = true;
    tracked_count = tracked_objects.size();

    allocation_count++;
    total_allocated++;
    estimated_memory_usage += est;

    if (tracked_count > peak_tracked) {
        peak_tracked = tracked_count;
    }
    if (memory_target > 0 && estimated_memory_usage > memory_target) {
        trigger_collection = true;
    }
    if (tracked_count >= collection_target) {
        trigger_collection = true;
    }
}

void GarbageCollector::untrack(HeapHeader *p) {
    if (!p || !p->gc_tracked) {
        return;
    }
    const size_t index = p->gc_index;
    HeapHeader *last = tracked_objects.back();
    if (index + 1 != tracked_objects.size()) {
        tracked_objects[index] = last;
        last->gc_index = index;
    }
    tracked_objects.pop_back();
    p->gc_tracked = false;
    size_t est = p->gc_est;
    estimated_memory_usage = (estimated_memory_usage >= est) ? estimated_memory_usage - est : 0;
    tracked_count = tracked_objects.size();
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
    for (HeapHeader *p : tracked_objects) {
        p->gc_marked = false;
    }
}

// iterative mark: an explicit worklist keeps C-stack usage independent of data shape
void GarbageCollector::mark_value(const Value &root) {
    auto push = [this](const Value &v) {
        if (v.is_heap()) {
            HeapHeader *p = v.heap_ptr();
            if (p && !p->gc_marked) {
                p->gc_marked = true;
                mark_stack.push_back(v.raw_bits());
            }
        }
    };
    push(root);
    while (!mark_stack.empty()) {
        const Value v = Value::from_raw(mark_stack.back());
        mark_stack.pop_back();
        HeapHeader *p = v.heap_ptr();
        switch (p->type_tag) {
            case ValueTag::String:
                push(((StringObj *)p)->getter_key_cache);
                break;
            case ValueTag::Array:
                for (const auto &elem : ((ArrayObj *)p)->v) {
                    push(elem);
                }
                if (auto *properties = ((ArrayObj *)p)->properties.get()) {
                    for (const Value &field : properties->fields) {
                        push(field);
                    }
                    push(properties->lazy_payload);
                    if (properties->dict_mode) {
                        for (const auto &entry : properties->dict_fields()) {
                            push(entry.second);
                        }
                    }
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
                    for (const auto &entry : obj->dict_fields()) {
                        push(entry.second);
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
                auto *fd = ((FunctionData *)p);
                if (fd->captures) {
                    for (const auto &cell : *fd->captures) {
                        if (cell) {
                            push(*cell);
                        }
                    }
                }
                if (fd->func_ptr && fd->func_ptr->closure_env_ptr) {
                    const auto &closure_env = *(const std::shared_ptr<std::map<std::string, Value>> *)fd->func_ptr->closure_env_ptr;
                    if (closure_env && marked_closure_environments.insert(closure_env.get()).second) {
                        for (const auto &[name, value] : *closure_env) {
                            push(value);
                        }
                    }
                }
                if (fd->func_ptr && fd->func_ptr->closure_owner_env_ptr) {
                    using ClosureOwnerMap = std::map<std::string, std::shared_ptr<std::map<std::string, Value>>>;
                    const auto &owners = *(const std::shared_ptr<ClosureOwnerMap> *)fd->func_ptr->closure_owner_env_ptr;
                    if (owners) {
                        for (const auto &[name, environment] : *owners) {
                            if (environment && marked_closure_environments.insert(environment.get()).second) {
                                for (const auto &[owner_name, value] : *environment) {
                                    push(value);
                                }
                            }
                        }
                    }
                }
                if (fd->properties) {
                    for (const Value &field : fd->properties->fields) {
                        push(field);
                    }
                    push(fd->properties->lazy_payload);
                    if (fd->properties->dict_mode) {
                        for (const auto &entry : fd->properties->dict_fields()) {
                            push(entry.second);
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
                break;
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
    
    static const size_t kGcPrefetch = []() -> size_t {
        if (const char *e = getenv("NARI_GC_PREFETCH")) {
            long v = strtol(e, nullptr, 10);
            if (v >= 0 && v < 4096) {
                return (size_t)v;
            }
        }
        return 64;
    }();
    const auto pf = [this](size_t i) {
        if (i < tracked_objects.size()) {
            __builtin_prefetch(tracked_objects[i], 1, 1);
        }
    };

    // Partition in place so large collections do not allocate a second pointer array.
    size_t live_count = 0;
    size_t garbage_begin = tracked_objects.size();
    while (live_count < garbage_begin) {
        pf(live_count + kGcPrefetch);
        if (garbage_begin >= kGcPrefetch) {
            pf(garbage_begin - kGcPrefetch);
        }
        if (tracked_objects[live_count]->gc_marked) {
            live_count++;
            continue;
        }
        do {
            garbage_begin--;
        } while (live_count < garbage_begin && !tracked_objects[garbage_begin]->gc_marked);
        if (live_count == garbage_begin) {
            break;
        }
        std::swap(tracked_objects[live_count], tracked_objects[garbage_begin]);
        live_count++;
    }

    for (size_t i = 0; i < live_count; i++) {
        pf(i + kGcPrefetch);
        HeapHeader *p = tracked_objects[i];
        p->gc_index = i;
        p->gc_marked = false;
    }
    const size_t garbage_count = tracked_objects.size() - live_count;
    tracked_count = live_count;

    // clear container contents (breaks cycles)
    for (size_t i = live_count; i < tracked_objects.size(); i++) {
        pf(i + kGcPrefetch);
        HeapHeader *g = tracked_objects[i];
        g->gc_tracked = false;
        const size_t est = g->gc_est;
        estimated_memory_usage = (estimated_memory_usage >= est) ? estimated_memory_usage - est : 0;
        switch (g->type_tag) {
            case ValueTag::Array:
                ((ArrayObj *)g)->v.clear();
                ((ArrayObj *)g)->properties.reset();
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
                ((FunctionData *)g)->properties.reset();
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

    // Delete from the back so each removed registry entry can be popped immediately.
    while (tracked_objects.size() > live_count) {
        if (tracked_objects.size() > live_count + kGcPrefetch) {
            __builtin_prefetch(tracked_objects[tracked_objects.size() - 1 - kGcPrefetch], 1, 1);
        }
        HeapHeader *g = tracked_objects.back();
        tracked_objects.pop_back();
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

    return garbage_count;
}

// full collection
size_t GarbageCollector::collect(const std::vector<const Value *> &roots) {
    if (!enabled) {
        return 0;
    }
    total_collections++;
    static const bool gc_profile = getenv("NARI_GC_PROFILE") != nullptr;
    std::chrono::steady_clock::time_point t0, t1, t2;
    size_t tracked_before = tracked_objects.size();
    if (gc_profile) t0 = std::chrono::steady_clock::now();
    marked_closure_environments.clear();
    for (const Value *root : roots) {
        if (root) {
            mark_value(*root);
        }
    }
    if (gc_profile) t1 = std::chrono::steady_clock::now();
    // don't retain a pathologically grown worklist across collections (512KB cap)
    if (mark_stack.capacity() > 65536) {
        mark_stack.shrink_to_fit();
    }
    size_t collected = sweep();
    if (gc_profile) {
        t2 = std::chrono::steady_clock::now();
        auto ms = [](std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        fprintf(stderr, "[gcprof] #%zu tracked_before=%zu live=%zu roots=%zu mark=%.2fms sweep=%.2fms\n",
                (size_t)total_collections, tracked_before, tracked_count, roots.size(), ms(t0, t1), ms(t1, t2));
    }
    total_collected += collected;
    reset_allocation_count();
    collection_target = tracked_count + std::max(collection_threshold, tracked_count);
    if (memory_limit > 0) {
        memory_target = std::max(memory_limit, estimated_memory_usage * 2);
    }
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
    return Stats { tracked_count, allocation_count, total_collections, total_collected,
                   total_allocated, peak_tracked, collection_threshold, enabled };
}
