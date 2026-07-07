#include "core_types.h"
#include "gc.h"

#include <new>
#include <stdlib.h>
#include <vector>

// Heap-object header free-list pools
// A bounded free-list recycles just the fixed-size header block, the C++ members
namespace {
class HeaderPool {
  public:
    explicit HeaderPool(std::size_t cap)
        : slots(new std::vector<void *>()), cap(cap) {
        this->slots->reserve(cap);
    }

    void *allocate(std::size_t size) {
        if (!this->slots->empty()) {
            void *ptr = this->slots->back();
            this->slots->pop_back();
            return ptr;
        }
        void *ptr = malloc(size);
        if (!ptr) {
            throw std::bad_alloc();
        }
        return ptr;
    }

    void deallocate(void *ptr) noexcept {
        if (!ptr) {
            return;
        }
        if (this->slots->size() < this->cap) {
            this->slots->push_back(ptr);
            return;
        }
        std::free(ptr);
    }

  private:
    std::vector<void *> *slots;
    std::size_t cap;
};

constexpr std::size_t HeaderPoolCap = 8192;

// One pool per pooled type, selected at compile time. The pool is private to each TU and lazily constructed.
template <class Derived>
HeaderPool &pool_for() {
    static HeaderPool *p = new HeaderPool(HeaderPoolCap);
    return *p;
}
} // namespace

template <class Derived>
void *PooledHeapObject<Derived>::operator new(std::size_t sz) {
    return pool_for<Derived>().allocate(sz);
}
template <class Derived>
void PooledHeapObject<Derived>::operator delete(void *ptr) noexcept {
    pool_for<Derived>().deallocate(ptr);
}

template struct PooledHeapObject<StringObj>;
template struct PooledHeapObject<ArrayObj>;
template struct PooledHeapObject<ObjectObj>;

Value Value::make_array() {
    Value val;
    auto *arr = new ArrayObj();
    arr->type_tag = ValueTag::Array;
    GarbageCollector::instance().track(arr, GarbageCollector::TrackedType::Array);
    val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(arr);
    return val;
}

Value Value::make_array(std::vector<Value> elements) {
    Value val;
    auto *arr = new ArrayObj();
    arr->type_tag = ValueTag::Array;
    arr->v = std::move(elements);
    GarbageCollector::instance().track(arr, GarbageCollector::TrackedType::Array);
    val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(arr);
    return val;
}

Value Value::make_object() {
    Value val;
    auto *obj = new ObjectObj();
    obj->type_tag = ValueTag::Object;
    GarbageCollector::instance().track(obj, GarbageCollector::TrackedType::Object);
    val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(obj);
    return val;
}

Value Value::make_object(ObjectObj *entries) {
    Value val;
    if (entries) {
        // entries is already tracked by the collector; just wrap it.
        val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(entries);
    } else {
        auto *obj = new ObjectObj();
        obj->type_tag = ValueTag::Object;
        GarbageCollector::instance().track(obj, GarbageCollector::TrackedType::Object);
        val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(obj);
    }
    return val;
}

HandlePtr Value::make_handle_ptr() {
    auto *handle = new HandleData();
    handle->type_tag = ValueTag::Handle;
    GarbageCollector::instance().track(handle, GarbageCollector::TrackedType::Handle);
    return handle;
}

Value Value::make_handle(HandlePtr handlePtr) {
    Value val;
    if (handlePtr) {
        val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(handlePtr);
    }
    return val;
}

struct ObjectDict {
    std::unordered_map<std::string, Value> map;
};

void delete_object_dict(ObjectDict *p) noexcept {
    delete p;
}

ObjectObj::ObjectObj() : shape(object_shape_registry().empty_shape()) {
}

std::vector<std::string> ObjectObj::get_keys() const {
    std::vector<std::string> out;
    if (dict_mode && dict) {
        out.reserve(dict->map.size());
        for (const auto &kv : dict->map) {
            out.push_back(kv.first);
        }
    } else {
        out = shape->names;
    }
    return out;
}

void ObjectObj::clear_fields() {
    fields.clear();
    if (dict) {
        dict->map.clear();
    }
}

Value *ObjectObj::get_field(const std::string &name) noexcept {
    if (dict_mode && dict) {
        auto it = dict->map.find(name);
        return it != dict->map.end() ? &it->second : nullptr;
    }
    auto it = shape->index.find(intern_field(name));
    return it != shape->index.end() ? &fields[it->second] : nullptr;
}

const Value *ObjectObj::get_field(const std::string &name) const noexcept {
    if (dict_mode && dict) {
        auto it = dict->map.find(name);
        return it != dict->map.end() ? &it->second : nullptr;
    }
    auto it = shape->index.find(intern_field(name));
    return it != shape->index.end() ? &fields[it->second] : nullptr;
}

const Value *ObjectObj::get_field_by_id(uint32_t fid) const noexcept {
    if (dict_mode && dict) {
        // Rare (only for >32-field objects). Recover the name and use the
        // string path so dict-mode semantics are preserved exactly.
        auto it = dict->map.find(field_name(fid));
        return it != dict->map.end() ? &it->second : nullptr;
    }
    auto it = shape->index.find(fid);
    return it != shape->index.end() ? &fields[it->second] : nullptr;
}

void ObjectObj::promote_to_dict_mode() {
    if (dict_mode) {
        return;
    }
    // move existing shape-mode fields into the hash map, preserving values.
    dict.reset(new ObjectDict());
    const auto &names = shape->names;
    dict->map.reserve(names.size() * 2);
    for (size_t i = 0; i < names.size() && i < fields.size(); i++) {
        dict->map.emplace(names[i], std::move(fields[i]));
    }
    fields.clear();
    fields.shrink_to_fit();
    // reset to the empty shape so any code that still reads `shape` sees a sane layout
    shape = object_shape_registry().empty_shape();
    dict_mode = true;
    shape_version++;
}

void ObjectObj::set_field(const std::string &name, Value val) {
    if (frozen) {
        return;
    }
    if (dict_mode) {
        if (!dict) {
            dict.reset(new ObjectDict());
        }
        auto it = dict->map.find(name);
        if (it != dict->map.end()) {
            it->second = std::move(val);
        } else {
            dict->map.emplace(name, std::move(val));
            shape_version++;
        }
        return;
    }
    uint32_t fid = intern_field(name);
    auto it = shape->index.find(fid);
    if (it != shape->index.end()) {
        fields[it->second] = std::move(val);
        return;
    }
    // if we'd exceed the dict-mode threshold, promote first
    if (shape->names.size() >= kDictModeThreshold) {
        promote_to_dict_mode();
        dict->map.emplace(name, std::move(val));
        shape_version++;
        return;
    }
    shape = object_shape_registry().extend(shape, fid);
    fields.push_back(std::move(val));
    shape_version++;
}

bool ObjectObj::has_field(const std::string &name) const noexcept {
    if (dict_mode && dict) {
        return dict->map.count(name) != 0;
    }
    return shape->index.count(intern_field(name)) != 0;
}

bool ObjectObj::is_empty() const noexcept {
    if (dict_mode) {
        return !dict || dict->map.empty();
    }
    return fields.empty();
}

size_t ObjectObj::field_count() const noexcept {
    if (dict_mode) {
        return dict ? dict->map.size() : 0;
    }
    return fields.size();
}

// ClassInstance
ClassInstance::ClassInstance(std::string cn) : class_name(std::move(cn)) {
    type_tag = ValueTag::ClassInstance;
}

Value *ClassInstance::get_field(const std::string &name) noexcept {
    if (!layout) {
        return nullptr;
    }
    auto fit = layout->index.find(name);
    if (fit == layout->index.end()) {
        return nullptr;
    }
    uint32_t idx = fit->second;
    if (idx >= static_cast<uint32_t>(field_values.size())) {
        return nullptr;
    }
    return &field_values[idx];
}

const Value *ClassInstance::get_field(const std::string &name) const noexcept {
    if (!layout) {
        return nullptr;
    }
    auto fit = layout->index.find(name);
    if (fit == layout->index.end()) {
        return nullptr;
    }
    uint32_t idx = fit->second;
    if (idx >= static_cast<uint32_t>(field_values.size())) {
        return nullptr;
    }
    return &field_values[idx];
}
