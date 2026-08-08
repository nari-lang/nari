#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast.h"
#include "compiler_support.h"
#include "gc.h"

namespace chrono = std::chrono;
using namespace nari;

typedef std::map<std::string, std::string> StringMap;

struct Value;
template <class T> class OwnedArray;
using Array = OwnedArray<Value>;
struct Task;
struct HeapHeader;
struct StringObj;
struct ArrayObj;
struct ObjectObj;
struct FunctionData;
struct HandleData;
struct ClassInstance;
struct RegexObj;
struct DelegateData;
struct ObjectDict;
struct ObjectShape;
struct ClassLayout;

namespace nari {
struct Function;
namespace bytecode {
struct FunctionMeta;
}
} // namespace nari

using Object = ObjectObj *;
using HandlePtr = HandleData *;
using ClassInstancePtr = ClassInstance *;
// Non-atomic reference-counted cell, used for closure upvalues.
//
// Thread-local pool for small, short-lived runtime allocations.
struct SmallBufferPool {
    static constexpr std::size_t kGranule = 16; // >= sizeof(void*), so a free block can hold its link
    static constexpr std::size_t kMaxBytes = 512;
    static constexpr std::size_t kClasses = kMaxBytes / kGranule;
    static constexpr std::size_t kChunkBytes = 64 * 1024;

    void *free_list[kClasses] = {};
    char *bump = nullptr;
    char *bump_end = nullptr;

    // Precondition: 1 <= bytes <= kMaxBytes, giving classes 0..31.
    static std::size_t class_index(std::size_t bytes) noexcept {
        return (bytes + kGranule - 1) / kGranule - 1;
    }
    void *allocate(std::size_t bytes) {
        const std::size_t ci = class_index(bytes);
        void *&head = free_list[ci];
        if (head) {
            void *p = head;
            head = *reinterpret_cast<void **>(p);
            return p;
        }
        const std::size_t need = (ci + 1) * kGranule;
        if ((std::size_t)(bump_end - bump) < need) {
            // Abandon the tail of the current chunk (< 512 B of a 64 KiB chunk).
            bump = static_cast<char *>(::operator new(kChunkBytes));
            bump_end = bump + kChunkBytes;
        }
        void *p = bump;
        bump += need;
        return p;
    }
    void deallocate(void *p, std::size_t bytes) noexcept {
        void *&head = free_list[class_index(bytes)];
        *reinterpret_cast<void **>(p) = head;
        head = p;
    }
};
inline SmallBufferPool &small_buffer_pool() noexcept {
    static thread_local SmallBufferPool pool;
    return pool;
}

// These cells were CellRef. Script execution is single-threaded, so an atomic refcount is excessive
// The layout is part of the JIT ABI and must not change!
template <class T>
class Rc {
    struct Box {
        uint32_t n; // first member, so a Box* and a &Box::n share an address
        T v;
    };
    T *ptr = nullptr;
    uint32_t *cnt = nullptr;

    void release() noexcept {
        if (cnt && --*cnt == 0) {
            delete reinterpret_cast<Box *>(cnt);
        }
    }

  public:
    Rc() noexcept = default;
    template <class... Args> static Rc make(Args &&...args) {
        Box *b = new Box{ 1u, T(std::forward<Args>(args)...) };
        Rc r;
        r.ptr = &b->v;
        r.cnt = &b->n;
        return r;
    }
    Rc(const Rc &o) noexcept : ptr(o.ptr), cnt(o.cnt) {
        if (cnt) {
            ++*cnt;
        }
    }
    Rc(Rc &&o) noexcept : ptr(o.ptr), cnt(o.cnt) {
        o.ptr = nullptr;
        o.cnt = nullptr;
    }
    Rc &operator=(const Rc &o) noexcept {
        if (this != &o) {
            if (o.cnt) {
                ++*o.cnt;
            }
            release();
            ptr = o.ptr;
            cnt = o.cnt;
        }
        return *this;
    }
    Rc &operator=(Rc &&o) noexcept {
        if (this != &o) {
            release();
            ptr = o.ptr;
            cnt = o.cnt;
            o.ptr = nullptr;
            o.cnt = nullptr;
        }
        return *this;
    }
    ~Rc() {
        release();
    }
    void reset() noexcept {
        release();
        ptr = nullptr;
        cnt = nullptr;
    }
    T *get() const noexcept {
        return ptr;
    }
    T &operator*() const noexcept {
        return *ptr;
    }
    T *operator->() const noexcept {
        return ptr;
    }
    explicit operator bool() const noexcept {
        return ptr != nullptr;
    }
    bool operator==(const Rc &o) const noexcept {
        return ptr == o.ptr;
    }
    bool operator!=(const Rc &o) const noexcept {
        return ptr != o.ptr;
    }
};
using CellRef = Rc<Value>;
using CapturesList = std::shared_ptr<std::vector<CellRef>>;
using FuncList = std::vector<nari::FunctionPtr>;

enum class ValueTag : uint8_t {
    String = 4,
    Array = 5,
    Object = 6,
    Function = 7,
    Handle = 8,
    ClassInstance = 9,
    Regex = 10,
    Delegate = 11,
};

enum class JitInlineKind : int32_t {
    None = 0,
    IntAdd = 1,
    IntSub = 2,
    IntMul = 3,
    MulConst = 4,
    AddConst = 5,
    SubConst = 6,
    ClosureInc = 7,
    ClosureAddConst = 8,
    Identity = 9,
    Negate = 10,
    LT = 11,
    LE = 12,
    GT = 13,
    GE = 14,
    EQ = 15,
    NE = 16,
    Capture0 = 17,
};

inline int32_t to_int(JitInlineKind kind) {
    return (int32_t)kind;
}

struct HeapHeader {
    // Objects are reclaimed solely by the mark-sweep GC.
    ValueTag type_tag = ValueTag::String;
    bool gc_marked = false;
    bool gc_tracked = false;
    uint32_t gc_est = 0;
    size_t gc_index = 0;
};

template <class Derived> struct PooledHeapObject {
    static void *operator new(std::size_t sz);
    static void operator delete(void *ptr) noexcept;
    // Keep placement-new usable (jit_layout.h probes layout with `new (buf) T{}`);
    // declaring the sized operators above would otherwise hide it.
    static void *operator new(std::size_t, void *p) noexcept {
        return p;
    };
    static void operator delete(void *, void *) noexcept {
    }
};

struct Value {
    static constexpr uint64_t NB_HEAP_TAG = 0xFFFB000000000000ULL;
    static constexpr uint64_t NB_INT_TAG = 0xFFFC000000000000ULL;
    static constexpr uint64_t NB_BOOL_TAG = 0xFFFE000000000000ULL;
    static constexpr uint64_t NB_NONE = 0xFFFF000000000000ULL;
    static constexpr uint64_t PTR_MASK = 0x0000FFFFFFFFFFFFULL;
    static constexpr uint16_t TAG_HEAP = 0xFFFB;
    static constexpr uint16_t TAG_INT = 0xFFFC;
    static constexpr uint16_t TAG_BOOL = 0xFFFE;
    static constexpr uint16_t TAG_NONE = 0xFFFF;
    static constexpr int64_t INT48_MIN = -(1LL << 47);
    static constexpr int64_t INT48_MAX = (1LL << 47) - 1;

    uint64_t _raw = NB_NONE;

    Value() = default;
    Value(const Value &o) = default;
    // Value is a NaN-boxed uint64_t with no ownership (heap lifetime is managed
    // solely by the mark-sweep GC). Moves are therefore plain copies: defaulting
    // them keeps Value trivially copyable, so std::vector growth / frame setup can
    // relocate elements with memcpy instead of an element-wise move loop.
    Value(Value &&o) noexcept = default;
    Value &operator=(const Value &o) = default;
    Value &operator=(Value &&o) noexcept = default;

    static Value from_raw(uint64_t raw);
    uint64_t raw_bits() const;

    static Value none();
    static bool fits_int48(int64_t v);
    static Value make_int(int64_t v);
    static Value make_int_checked(int64_t v);
    static Value make_float(double v);
    static Value make_bool(bool v);
    static Value make_string(std::string v);
    static Value make_const_string(const std::string &v);
    static Value make_array();
    static Value make_array(std::vector<Value> elements);
    static Value make_array(const Value *elements, size_t count);
    static Value make_object();
    static Value make_object(ObjectObj *entries);
    static Value make_function(std::string name);
    static Value make_function(std::string name, std::shared_ptr<nari::Function> func_ptr);
    static HandlePtr make_handle_ptr();
    static Value make_handle(HandlePtr h);
    static Value from_class_instance(ClassInstancePtr p);
    static Value make_regex(std::string pattern, std::string flags);
    static Value make_delegate(Value target, Value handler);

    uint16_t tag_word() const;
    bool is_none() const;
    bool is_int() const;
    bool is_bool() const;
    bool is_float() const;
    bool is_numeric() const;
    bool is_heap() const;
    HeapHeader *heap_ptr() const;
    ValueTag heap_tag() const;
    bool is_string() const;
    bool is_array() const;
    bool is_object() const;
    bool is_function() const;
    bool is_handle() const;
    bool is_class_instance() const;
    bool is_regex() const;
    bool is_delegate() const;
    bool is_sso() const;
    uint8_t sso_len() const;
    char sso_char(uint8_t) const;
    bool is_mutable_heap_string() const;

    int64_t get_int() const;
    // Raw 48-bit payload, ZERO-extended.
    uintptr_t get_ptr_bits() const;
    void *get_ptr() const;
    double get_float() const;
    bool get_bool() const;
    const std::string &get_string() const;
    std::string &get_string();
    const Array &get_array() const;
    Array &get_array();
    ObjectObj *get_obj_ptr();
    const ObjectObj *get_obj_ptr() const;
    FunctionData &get_function();
    const FunctionData &get_function() const;
    HandlePtr get_handle() const;
    ClassInstancePtr get_class_instance() const;
    RegexObj *get_regex();
    const RegexObj *get_regex() const;
    DelegateData *get_delegate() const;

    void set_int(int64_t v);
    void set_int_checked(int64_t v);
    void set_bool(bool v);
    void set_float(double v);
    size_t tag() const;

    double as_number() const;
    bool as_bool() const;
    std::string to_string(bool in_container_context = false) const;
    std::string to_string_impl(bool in_container_context, std::vector<const void *> &seen) const;
    static bool values_equal(const Value &a, const Value &b);
    static bool values_strict_equal(const Value &a, const Value &b);
    static bool compare_ordered(const Value &a, const Value &b, int &cmp);
    static bool values_lt(const Value &a, const Value &b);
    static bool values_le(const Value &a, const Value &b);
    static bool values_gt(const Value &a, const Value &b);
    static bool values_ge(const Value &a, const Value &b);
};

static_assert(sizeof(Value) == sizeof(uint64_t), "Value must stay NaN-boxed to 8 bytes!");
// Keep Value trivially copyable for generated stack and frame operations.
static_assert(std::is_trivially_copyable<Value>::value, "Value must stay trivially copyable for memcpy relocation");

// owned contiguous storage with an explicit layout shared with generated code.
template <class T> class OwnedArray {
  public:
    using value_type = T;
    using size_type = std::size_t;
    using iterator = T *;
    using const_iterator = const T *;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    T *storage_begin;
    T *storage_end;
    T *storage_capacity;

    OwnedArray() noexcept : storage_begin(empty_storage()), storage_end(storage_begin), storage_capacity(storage_begin) {
    }
    OwnedArray(const OwnedArray &other) : OwnedArray() {
        reserve(other.size());
        std::copy(other.begin(), other.end(), storage_begin);
        storage_end = storage_begin + other.size();
    }
    OwnedArray(OwnedArray &&other) noexcept
        : storage_begin(other.storage_begin), storage_end(other.storage_end), storage_capacity(other.storage_capacity) {
        other.reset_empty();
    }
    explicit OwnedArray(std::vector<T> elements) : OwnedArray() {
        reserve(elements.size());
        std::move(elements.begin(), elements.end(), storage_begin);
        storage_end = storage_begin + elements.size();
    }
    OwnedArray &operator=(const OwnedArray &other) {
        if (this != &other) {
            OwnedArray copy(other);
            swap(copy);
        }
        return *this;
    }
    OwnedArray &operator=(OwnedArray &&other) noexcept {
        if (this != &other) {
            release();
            storage_begin = other.storage_begin;
            storage_end = other.storage_end;
            storage_capacity = other.storage_capacity;
            other.reset_empty();
        }
        return *this;
    }
    OwnedArray &operator=(std::vector<T> elements) {
        OwnedArray replacement(std::move(elements));
        swap(replacement);
        return *this;
    }
    ~OwnedArray() noexcept {
        release();
    }

    bool empty() const noexcept {
        return storage_end == storage_begin;
    }
    size_type size() const noexcept {
        return (size_type)(storage_end - storage_begin);
    }
    size_type capacity() const noexcept {
        return (size_type)(storage_capacity - storage_begin);
    }
    T *data() noexcept {
        return storage_begin;
    }
    const T *data() const noexcept {
        return storage_begin;
    }
    iterator begin() noexcept {
        return storage_begin;
    }
    const_iterator begin() const noexcept {
        return storage_begin;
    }
    const_iterator cbegin() const noexcept {
        return storage_begin;
    }
    iterator end() noexcept {
        return storage_end;
    }
    const_iterator end() const noexcept {
        return storage_end;
    }
    const_iterator cend() const noexcept {
        return storage_end;
    }
    reverse_iterator rbegin() noexcept {
        return reverse_iterator(end());
    }
    const_reverse_iterator rbegin() const noexcept {
        return const_reverse_iterator(end());
    }
    reverse_iterator rend() noexcept {
        return reverse_iterator(begin());
    }
    const_reverse_iterator rend() const noexcept {
        return const_reverse_iterator(begin());
    }
    T &operator[](size_type index) noexcept {
        return storage_begin[index];
    }
    const T &operator[](size_type index) const noexcept {
        return storage_begin[index];
    }
    T &back() noexcept {
        return storage_end[-1];
    }
    const T &back() const noexcept {
        return storage_end[-1];
    }

    void clear() noexcept {
        while (!empty()) {
            pop_back();
        }
    }
    void reserve(size_type requested) {
        if (requested <= capacity()) {
            return;
        }
        if (requested > max_size()) {
            throw std::length_error("owned array capacity exceeds maximum size");
        }
        const size_type old_size = size();
        const size_type old_capacity = capacity();
        T *replacement = allocate_storage(requested);
        if constexpr (std::is_trivially_copyable<T>::value) {
            std::memcpy(replacement, storage_begin, old_size * sizeof(T));
        } else {
            for (size_type i = 0; i < old_size; ++i) {
                replacement[i] = std::move(storage_begin[i]);
            }
        }
        if (storage_begin != empty_storage()) {
            free_storage(storage_begin, old_capacity);
        }
        storage_begin = replacement;
        storage_end = replacement + old_size;
        storage_capacity = replacement + requested;
    }
    void shrink_to_fit() {
        if (size() == capacity()) {
            return;
        }
        OwnedArray replacement;
        replacement.reserve(size());
        for (T &value : *this) {
            replacement.push_back(std::move(value));
        }
        swap(replacement);
    }
    NARI_ALWAYS_INLINE void resize(size_type requested) {
        const size_type old_size = size();
        if (requested <= old_size) {
            if constexpr (std::is_trivially_destructible<T>::value) {
                storage_end = storage_begin + requested;
            } else {
                while (size() > requested) {
                    pop_back();
                }
            }
            return;
        }
        grow_to(requested);
    }
    void grow_to(size_type requested) {
        if (requested > capacity()) {
            reserve(growth_capacity(requested));
        }
        std::fill(storage_end, storage_begin + requested, T{});
        storage_end = storage_begin + requested;
    }
    void resize(size_type requested, const T &value) {
        T copy = value;
        const size_type old_size = size();
        if (requested <= old_size) {
            while (size() > requested) {
                pop_back();
            }
            return;
        }
        if (requested > capacity()) {
            reserve(growth_capacity(requested));
        }
        std::fill(storage_end, storage_begin + requested, copy);
        storage_end = storage_begin + requested;
    }
    void assign(size_type count, const T &value) {
        clear();
        resize(count, value);
    }
    void push_back(const T &value) {
        T copy = value;
        ensure_one_more();
        *storage_end++ = std::move(copy);
    }
    void push_back(T &&value) {
        ensure_one_more();
        *storage_end++ = std::move(value);
    }
    template <class... Args> T &emplace_back(Args &&...args) {
        ensure_one_more();
        *storage_end = T(std::forward<Args>(args)...);
        return *storage_end++;
    }
    void pop_back() noexcept {
        --storage_end;
        if constexpr (!std::is_trivially_destructible<T>::value) {
            *storage_end = T{};
        }
    }
    iterator insert(const_iterator pos, const T &value) {
        const size_type index = pos - storage_begin;
        T copy = value;
        ensure_one_more();
        std::move_backward(storage_begin + index, storage_end, storage_end + 1);
        storage_begin[index] = std::move(copy);
        ++storage_end;
        return storage_begin + index;
    }
    iterator insert(const_iterator pos, const_iterator first, const_iterator last) {
        const size_type index = pos - storage_begin;
        std::vector<T> copy(first, last);
        const size_type requested = checked_size(copy.size());
        if (requested > capacity()) {
            reserve(growth_capacity(requested));
        }
        std::move_backward(storage_begin + index, storage_end, storage_end + copy.size());
        std::move(copy.begin(), copy.end(), storage_begin + index);
        storage_end += copy.size();
        return storage_begin + index;
    }
    iterator erase(const_iterator first, const_iterator last) noexcept {
        const size_type first_index = first - storage_begin;
        const size_type removed = last - first;
        std::move(storage_begin + first_index + removed, storage_end, storage_begin + first_index);
        for (size_type i = 0; i < removed; ++i) {
            pop_back();
        }
        return storage_begin + first_index;
    }
    void swap(OwnedArray &other) noexcept {
        std::swap(storage_begin, other.storage_begin);
        std::swap(storage_end, other.storage_end);
        std::swap(storage_capacity, other.storage_capacity);
    }

  private:
    // Only pool types that can live in raw storage and need no destructor pass;
    // anything else (e.g. CallFrame, which owns CellRefs) keeps plain new[]/delete[].
    static constexpr bool kPooled =
        std::is_trivially_copyable<T>::value && std::is_trivially_destructible<T>::value && alignof(T) <= 16;

    // Allocates storage for `count` elements and default-initializes every one of
    // them, exactly as `new T[count]` did. The initialization is deliberately kept:
    // capacity beyond size() is guaranteed to hold well-formed values (NB_NONE for
    // Value), and the GC and interpreter are only safe to read a stale slot because
    // of that. Only the allocation is pooled.
    static T *allocate_storage(size_type count) {
        const std::size_t bytes = count * sizeof(T);
        if constexpr (kPooled) {
            if (bytes <= SmallBufferPool::kMaxBytes) {
                T *p = static_cast<T *>(small_buffer_pool().allocate(bytes));
                std::uninitialized_fill_n(p, count, T{});
                return p;
            }
        }
        return new T[count];
    }
    static void free_storage(T *p, size_type count) noexcept {
        if constexpr (kPooled) {
            const std::size_t bytes = count * sizeof(T);
            if (bytes <= SmallBufferPool::kMaxBytes) {
                small_buffer_pool().deallocate(p, bytes);
                return;
            }
        }
        delete[] p;
    }

    static T *empty_storage() noexcept {
        static T empty;
        return &empty;
    }
    static constexpr size_type max_size() noexcept {
        return std::numeric_limits<size_type>::max() / sizeof(T);
    }
    void reset_empty() noexcept {
        storage_begin = empty_storage();
        storage_end = storage_begin;
        storage_capacity = storage_begin;
    }
    void release() noexcept {
        if (storage_begin != empty_storage()) {
            free_storage(storage_begin, capacity());
        }
    }
    size_type checked_size(size_type added) const {
        if (added > max_size() - size()) {
            throw std::length_error("owned array size exceeds maximum size");
        }
        return size() + added;
    }
    size_type growth_capacity(size_type requested) const {
        if (requested > max_size()) {
            throw std::length_error("owned array capacity exceeds maximum size");
        }
        size_type grown = capacity() ? capacity() + capacity() / 2 : 4;
        if (grown < capacity() || grown > max_size()) {
            grown = max_size();
        }
        return std::max(requested, grown);
    }
    NARI_ALWAYS_INLINE void ensure_one_more() {
        if (NARI_UNLIKELY(storage_end == storage_capacity)) {
            grow_for_one_more();
        }
    }
    void grow_for_one_more() {
        reserve(growth_capacity(checked_size(1)));
    }
};

using ByteArray = OwnedArray<uint8_t>;

static_assert(std::is_standard_layout<Array>::value, "Array layout is part of the JIT ABI");
static_assert(std::is_standard_layout<ByteArray>::value, "ByteArray layout is part of the JIT ABI");

struct StringObj : HeapHeader, PooledHeapObject<StringObj> {
    std::string s;
    mutable Value getter_key_cache;
    mutable uint32_t field_id = UINT32_MAX;
    mutable uint32_t getter_field_id = UINT32_MAX;
    mutable uint32_t setter_field_id = UINT32_MAX;
    mutable const ObjectShape *cached_shape = nullptr;
    mutable uint32_t cached_slot = 0;
    bool immutable = false;
    bool js_getter_prefix = false;
    StringObj() {
        type_tag = ValueTag::String;
    }
    explicit StringObj(std::string v) : s(std::move(v)) {
        type_tag = ValueTag::String;
    }
    // StringObj is by far the most churned heap object (toString / split tokens / concat temps).
    // Every make_string() does one new StringObj and the GC sweep one delete.
    // The PooledHeapObject free-list recycles just the fixed-size header block
};

struct ArrayObj : HeapHeader, PooledHeapObject<ArrayObj> {
    Array v;
    std::unique_ptr<ObjectObj> properties;
    ArrayObj();
    ~ArrayObj();
    const Value *get_property(const std::string &name) const noexcept;
    bool has_property(const std::string &name) const noexcept;
    void set_property(const std::string &name, Value value);
    // Header free-list pool (see PooledHeapObject in core_types.cpp).
    // Only the fixed-size header block is recycled.
    // The owned array storage is constructed/destructed normally.
};

uint32_t intern_field(const std::string &name);
const std::string &field_name(uint32_t id);

inline const std::string &field_name(uint32_t id);

struct ObjectShape {
    std::vector<uint32_t> field_ids;
    mutable std::unordered_map<uint32_t, const ObjectShape *> transitions;
    uint64_t field_mask = 0;

    static constexpr uint32_t kNoSlot = UINT32_MAX;

    const std::string &name_at(size_t slot) const {
        return field_name(field_ids[slot]);
    }
    NARI_ALWAYS_INLINE uint32_t slot_of(uint32_t fid) const noexcept {
        if ((field_mask & (uint64_t{ 1 } << (fid & 63))) == 0) {
            return kNoSlot;
        }
        const size_t n = field_ids.size();
        const uint32_t *ids = field_ids.data();
        for (size_t i = 0; i < n; i++) {
            if (ids[i] == fid) {
                return (uint32_t)i;
            }
        }
        return kNoSlot;
    }
};

class ObjectShapeRegistry {
  public:
    ObjectShapeRegistry();
    const ObjectShape *empty_shape() const;
    const ObjectShape *extend(const ObjectShape *base, uint32_t fid);

  private:
    const ObjectShape *empty = nullptr;
    std::vector<std::unique_ptr<ObjectShape>> shapes;
};

std::unordered_map<std::string, uint32_t> &field_intern_map();
std::vector<std::string> &field_intern_names();
ObjectShapeRegistry &object_shape_registry();

void delete_object_dict(ObjectDict *p) noexcept;

struct ObjectObj : HeapHeader, PooledHeapObject<ObjectObj> {
    using LazyFieldFactory = Value (*)(void *, ObjectObj *, uint32_t);
    using LazyFieldInvoker = bool (*)(void *, ObjectObj *, uint32_t, const Value *, size_t, Value &);
    static constexpr size_t kDictModeThreshold = 64;

    const ObjectShape *shape = nullptr;
    Array fields;
    std::unique_ptr<ObjectDict, void (*)(ObjectDict *)> dict = { nullptr, delete_object_dict };
    std::string native_struct_type;
    std::vector<uint8_t> native_struct_storage;
    uint32_t shape_version = 0;
    void *lazy_field_context = nullptr;
    LazyFieldFactory lazy_field_factory = nullptr;
    LazyFieldInvoker lazy_field_invoker = nullptr;
    Value lazy_payload;
    CapturesList lazy_captures;
    uint64_t lazy_field_mask = 0;
    bool frozen = false;
    bool dict_mode = false;

    ObjectObj();
    // Header free-list pool (see PooledHeapObject in core_types.cpp).
    // Only the fixed-size header block is recycled
    std::vector<std::string> get_keys() const;
    void clear_fields();
    Value *get_field(const std::string &name) noexcept;
    const Value *get_field(const std::string &name) const noexcept;
    // Lookup by a pre-interned field id, skipping the per-call intern_field() string hash.
    Value *get_field_by_id(uint32_t fid) noexcept;
    const Value *get_field_by_id(uint32_t fid) const noexcept;
    Value *materialize_lazy_field(uint32_t slot);
    void clear_lazy_field(uint32_t slot) noexcept;
    bool invoke_lazy_field(uint32_t slot, const Value *args, size_t argc, Value &result);
    void promote_to_dict_mode();
    void set_field(const std::string &name, Value val);
    void set_field_by_id(uint32_t fid, Value val);
    bool has_field(const std::string &name) const noexcept;
    bool has_field_by_id(uint32_t fid) const noexcept;
    const std::unordered_map<std::string, Value> &dict_fields() const noexcept;
    bool is_empty() const noexcept;
    size_t field_count() const noexcept;
    bool is_managed_native_struct() const noexcept {
        return !native_struct_type.empty() && !native_struct_storage.empty();
    }
};

struct FunctionData : HeapHeader, PooledHeapObject<FunctionData> {
    // JIT call-path hot set, attempted to be ordered to minimise cache lines touched per call
    nari::bytecode::FunctionMeta *jit_meta = nullptr;
    int32_t jit_func_idx = -1;
    uint32_t jit_locals_count = 0;
    std::vector<CellRef> *jit_captures_raw = nullptr;
    Value *jit_capture0_raw = nullptr;
    Value *jit_capture1_raw = nullptr;
    Value *jit_capture2_raw = nullptr;
    int32_t jit_native_kind = 0;
    JitInlineKind jit_inline_kind = JitInlineKind::None;
    // mirrored FunctionMeta scalars (see #17): only valid when jit_meta != nullptr.
    uint8_t jit_param_count = 0;
    int8_t jit_rest_param_index = -1;
    bool jit_js_undefined_params = false;
    // 1-based index into ScriptRuntime::jit_builtin_table(); 0 == not a builtin.
    uint16_t jit_builtin_id = 0;
    int64_t jit_inline_imm = 0;

    // cold
    std::string name;
    std::shared_ptr<nari::Function> func_ptr;
    CapturesList captures;
    // JS-style callable objects need fields of their own. Keep the
    // table lazy so ordinary Nari functions retain their compact hot path.
    std::unique_ptr<ObjectObj> properties;
    void cache_jit_captures() {
        jit_captures_raw = captures.get();
        jit_capture0_raw = captures && !captures->empty() ? (*captures)[0].get() : nullptr;
        jit_capture1_raw = captures && captures->size() > 1 ? (*captures)[1].get() : nullptr;
        jit_capture2_raw = captures && captures->size() > 2 ? (*captures)[2].get() : nullptr;
    }
    // Pre-resolved runtime builtin member-function pointer for global builtins (toString, toNumber, etc.).

    FunctionData() {
        type_tag = ValueTag::Function;
    }
    explicit FunctionData(std::string n) : name(std::move(n)) {
        type_tag = ValueTag::Function;
    }
    FunctionData(std::string n, std::shared_ptr<nari::Function> ptr) : name(std::move(n)), func_ptr(std::move(ptr)) {
        type_tag = ValueTag::Function;
    }
    ObjectObj *ensure_properties();
    const Value *get_property(const std::string &name) const noexcept;
    void set_property(const std::string &name, Value val);
    // the callers all have an interned field id memoized already (VM::field_id_for_name / method_field_ids / StringObj::field_id)
    const Value *get_property_by_id(uint32_t fid) const noexcept;
    void set_property_by_id(uint32_t fid, Value val);

    // `fn.length` lives here rather than in `properties`.
    static bool is_length_name(const std::string &name) noexcept {
        return name.size() == 6 && name == "length";
    }
    Value int_length = Value::none();

    // use this instead of reading `properties` directly, because a set `length` lives in int_length and is absent there.
    bool has_property(const std::string &name) const noexcept {
        return get_property(name) != nullptr;
    }
    std::vector<std::string> property_keys() const;
};

struct ClassLayout {
    std::vector<std::string> names;
    std::unordered_map<std::string, uint32_t> index;
    std::unordered_set<std::string> private_fields;
};

typedef std::unordered_map<std::string, ClassLayout> LayoutRegistry;

LayoutRegistry &class_layout_registry();

struct ClassInstance : HeapHeader {
    std::string class_name;
    const ClassLayout *layout = nullptr;
    std::vector<Value> field_values;

    explicit ClassInstance(std::string cn);
    Value *get_field(const std::string &name) noexcept;
    const Value *get_field(const std::string &name) const noexcept;
};

struct RegexObj : HeapHeader {
    std::string pattern;
    std::string flags;
    RegexObj(std::string p, std::string f) : pattern(std::move(p)), flags(std::move(f)) {
        type_tag = ValueTag::Regex;
    }
};

// wraps a target value with a handler object whose
// optional get/set/has/call fields intercept the corresponding operations.
struct DelegateData : HeapHeader {
    Value target;
    Value handler;
    // delegate_trap() resolution cache
    const ObjectShape *trap_shape = nullptr;
    int32_t trap_slots[4] = { -1, -1, -1, -1 };
    DelegateData(Value t, Value h) : target(t), handler(h) {
        type_tag = ValueTag::Delegate;
    }
};

struct HandleData : HeapHeader {
    enum State {
        Running,
        Completed,
        Failed
    };
    State state = Running;
    Value result;
    Value error;
    std::unique_ptr<Task> task;
    chrono::steady_clock::time_point start_time;
    chrono::steady_clock::time_point end_time;

    HandleData();
};

struct Flags {
    bool break_flag = false;
    bool continue_flag = false;
    bool return_flag = false;
    Value return_value;
    bool tailcall_flag = false;
    std::vector<Value> tailcall_args;
    bool throw_flag = false;
    Value throw_value;
    bool shutdown_flag = false;

    bool any_flag() const {
        return break_flag || continue_flag || return_flag || throw_flag || shutdown_flag;
    }
};

struct IntervalData {
    int64_t id = 0;
    Value callback;
    int64_t interval_ms = 0;
    chrono::steady_clock::time_point next_fire;
};

struct Task {
    enum State {
        Running,
        Yielded,
        Completed,
        Failed
    };
    State state = Running;
    const nari::BlockStmt *body = nullptr;
    size_t current_stmt = 0;
    Value result;
    Value error;
    std::map<std::string, Value> locals;
    std::unordered_set<std::string> const_locals;
    std::vector<std::map<std::string, Value>> block_scopes;
    std::vector<std::unordered_set<std::string>> block_const_scopes;
    Flags flags;

    explicit Task(const nari::BlockStmt *b) : body(b) {
    }
};

inline ObjectShapeRegistry::ObjectShapeRegistry() {
    auto empty = std::make_unique<ObjectShape>();
    this->empty = empty.get();
    shapes.push_back(std::move(empty));
}

inline const ObjectShape *ObjectShapeRegistry::empty_shape() const {
    return this->empty;
}

inline const ObjectShape *ObjectShapeRegistry::extend(const ObjectShape *base, uint32_t fid) {
    if (!base) {
        base = this->empty;
    }
    if (base->slot_of(fid) != ObjectShape::kNoSlot) {
        return base;
    }

    auto it = base->transitions.find(fid);
    if (it != base->transitions.end()) {
        return it->second;
    }

    auto next = std::make_unique<ObjectShape>();
    next->field_ids = base->field_ids;
    next->field_mask = base->field_mask | (uint64_t{ 1 } << (fid & 63));
    next->field_ids.push_back(fid);
    const ObjectShape *ptr = next.get();
    this->shapes.push_back(std::move(next));
    base->transitions.emplace(fid, ptr);
    return ptr;
}

inline std::unordered_map<std::string, uint32_t> &field_intern_map() {
    static std::unordered_map<std::string, uint32_t> map;
    return map;
}

inline std::vector<std::string> &field_intern_names() {
    static std::vector<std::string> names;
    return names;
}

inline uint32_t intern_field_slow(const std::string &name) {
    auto &map = field_intern_map();
    auto it = map.find(name);
    if (it != map.end()) {
        return it->second;
    }
    auto &names = field_intern_names();
    uint32_t id = names.size();
    names.push_back(name);
    map.emplace(names.back(), id);
    return id;
}

inline uint32_t intern_field(const std::string &name) {
    // short names hit a direct-mapped cache keyed by the packed name bytes
    // this skips the string hash and bucket walk of field_intern_map.
    const size_t len = name.size();
    if (len == 0) {
        return intern_field_slow(name);
    }
    if (len - 1 < 8) { // 1..8 bytes (len 0 stays on the slow path)
        struct ShortNameEntry {
            uint64_t key;
            uint32_t len;
            uint32_t fid;
        };
        static ShortNameEntry short_cache[4096] = {};
        uint64_t key = 0;
        std::memcpy(&key, name.data(), len);
        ShortNameEntry &e = short_cache[(key * 0x9E3779B97F4A7C15ull >> 52) & 4095];
        if (e.key == key && e.len == len) {
            return e.fid;
        }
        uint32_t id = intern_field_slow(name);
        e.key = key;
        e.len = len;
        e.fid = id;
        return id;
    }

    // Repeated long property names are common in transpiled JavaScript. Avoid
    // hashing the whole string on a hit; equality keeps collisions exact.
    struct LongNameEntry {
        uint64_t key = 0;
        uint32_t len = 0;
        uint32_t fid = UINT32_MAX;
    };
    static LongNameEntry long_cache[4096];
    uint64_t first;
    uint64_t last;
    std::memcpy(&first, name.data(), sizeof(first));
    std::memcpy(&last, name.data() + len - sizeof(last), sizeof(last));
    uint64_t key = first ^ (last * 0x9E3779B97F4A7C15ull) ^ (len * 0xBF58476D1CE4E5B9ull);
    LongNameEntry &e = long_cache[(key ^ (key >> 32)) & 4095];
    if (e.key == key && e.len == len && e.fid != UINT32_MAX && field_name(e.fid) == name) {
        return e.fid;
    }
    uint32_t id = intern_field_slow(name);
    e.key = key;
    e.len = len;
    e.fid = id;
    return id;
}

inline const std::string &field_name(uint32_t id) {
    static const std::string empty;
    auto &names = field_intern_names();
    return id < names.size() ? names[id] : empty;
}

inline ObjectShapeRegistry &object_shape_registry() {
    static ObjectShapeRegistry reg;
    return reg;
}

inline LayoutRegistry &class_layout_registry() {
    static LayoutRegistry reg;
    return reg;
}

inline HandleData::HandleData() : start_time(chrono::steady_clock::now()) {
    this->type_tag = ValueTag::Handle;
}

inline Value Value::from_raw(uint64_t raw) {
    Value val;
    val._raw = raw;
    return val;
}
inline uint64_t Value::raw_bits() const {
    return this->_raw;
}
inline Value Value::none() {
    return Value();
}
inline bool Value::fits_int48(int64_t val) {
    return val >= INT48_MIN && val <= INT48_MAX;
}
inline Value Value::make_int(int64_t val) {
    Value new_val;
    new_val._raw = NB_INT_TAG | ((uint64_t)val & PTR_MASK);
    return new_val;
}
inline Value Value::make_int_checked(int64_t val) {
    return fits_int48(val) ? make_int(val) : make_float((double)val);
}
inline Value Value::make_float(double val) {
    Value new_val;
    std::memcpy(&new_val._raw, &val, sizeof(double));
    return new_val;
}
inline Value Value::make_bool(bool val) {
    Value new_val;
    new_val._raw = NB_BOOL_TAG | (val ? 1ULL : 0ULL);
    return new_val;
}
inline Value Value::make_string(std::string val) {
    Value new_val;
    auto *str = new StringObj(std::move(val));
    GarbageCollector::instance().track(str, GarbageCollector::TrackedType::String);
    new_val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(str);
    return new_val;
}
inline Value Value::make_const_string(const std::string &val) {
    Value new_val = make_string(val);
    if (auto *str = (StringObj *)new_val.heap_ptr()) {
        str->immutable = true;
        str->js_getter_prefix = val == "__js_getter__";
    }
    return new_val;
}
inline Value Value::make_function(std::string name) {
    Value val;
    auto *fn = new FunctionData(std::move(name));
    GarbageCollector::instance().track(fn, GarbageCollector::TrackedType::Function);
    val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(fn);
    return val;
}
inline Value Value::make_function(std::string name, std::shared_ptr<nari::Function> func_ptr) {
    Value val;
    auto *fn = new FunctionData(std::move(name), std::move(func_ptr));
    GarbageCollector::instance().track(fn, GarbageCollector::TrackedType::Function);
    val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(fn);
    return val;
}
inline Value Value::from_class_instance(ClassInstancePtr ptr) {
    Value val;
    if (ptr) {
        val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(ptr);
    }
    return val;
}
inline Value Value::make_regex(std::string pattern, std::string flags) {
    Value val;
    auto *regex = new RegexObj(std::move(pattern), std::move(flags));
    GarbageCollector::instance().track(regex, GarbageCollector::TrackedType::Unknown);
    val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(regex);
    return val;
}
inline Value Value::make_delegate(Value target, Value handler) {
    Value val;
    auto *delegate = new DelegateData(target, handler);
    GarbageCollector::instance().track(delegate, GarbageCollector::TrackedType::Unknown);
    val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(delegate);
    return val;
}
inline uint16_t Value::tag_word() const {
    return (uint16_t)(_raw >> 48);
}
inline bool Value::is_none() const {
    return _raw == NB_NONE;
}
inline bool Value::is_int() const {
    return tag_word() == TAG_INT;
}
inline bool Value::is_bool() const {
    return tag_word() == TAG_BOOL;
}
inline bool Value::is_float() const {
    uint16_t t = tag_word();
    return t != TAG_HEAP && t != TAG_INT && t != TAG_BOOL && t != TAG_NONE;
}
inline bool Value::is_numeric() const {
    return is_int() || is_float();
}
inline bool Value::is_heap() const {
    return tag_word() == TAG_HEAP;
}
inline HeapHeader *Value::heap_ptr() const {
    return is_heap() ? reinterpret_cast<HeapHeader *>(_raw & PTR_MASK) : nullptr;
}
inline ValueTag Value::heap_tag() const {
    HeapHeader *p = heap_ptr();
    return p ? p->type_tag : ValueTag::String;
}
inline bool Value::is_string() const {
    return is_heap() && heap_tag() == ValueTag::String;
}
inline bool Value::is_array() const {
    return is_heap() && heap_tag() == ValueTag::Array;
}
inline bool Value::is_object() const {
    return is_heap() && heap_tag() == ValueTag::Object;
}
inline bool Value::is_function() const {
    return is_heap() && heap_tag() == ValueTag::Function;
}
inline bool Value::is_handle() const {
    return is_heap() && heap_tag() == ValueTag::Handle;
}
inline bool Value::is_class_instance() const {
    return is_heap() && heap_tag() == ValueTag::ClassInstance;
}
inline bool Value::is_regex() const {
    return is_heap() && heap_tag() == ValueTag::Regex;
}
inline bool Value::is_delegate() const {
    return is_heap() && heap_tag() == ValueTag::Delegate;
}
inline bool Value::is_sso() const {
    return false;
}
inline uint8_t Value::sso_len() const {
    return 0;
}
inline char Value::sso_char(uint8_t) const {
    return '\0';
}
inline bool Value::is_mutable_heap_string() const {
    auto *ptr = heap_ptr();
    return ptr && ptr->type_tag == ValueTag::String && !((StringObj *)ptr)->immutable;
}
inline int64_t Value::get_int() const {
    int64_t val = (int64_t)(_raw & PTR_MASK);
    return (val << 16) >> 16;
}
inline uintptr_t Value::get_ptr_bits() const {
    return (uintptr_t)(_raw & PTR_MASK);
}
inline void *Value::get_ptr() const {
    return reinterpret_cast<void *>((uintptr_t)(_raw & PTR_MASK));
}
inline double Value::get_float() const {
    double d;
    uint64_t raw = _raw;
    std::memcpy(&d, &raw, sizeof(double));
    return d;
}
inline bool Value::get_bool() const {
    return (this->_raw & 1ULL) != 0;
}
inline const std::string &Value::get_string() const {
    static const std::string empty;
    auto *p = heap_ptr();
    return p && p->type_tag == ValueTag::String ? ((StringObj *)p)->s : empty;
}
inline std::string &Value::get_string() {
    static std::string empty;
    auto *p = heap_ptr();
    return p && p->type_tag == ValueTag::String ? ((StringObj *)p)->s : empty;
}
inline const Array &Value::get_array() const {
    static const Array empty;
    auto *p = heap_ptr();
    return p && p->type_tag == ValueTag::Array ? ((ArrayObj *)p)->v : empty;
}
inline Array &Value::get_array() {
    static Array empty;
    auto *p = heap_ptr();
    return p && p->type_tag == ValueTag::Array ? ((ArrayObj *)p)->v : empty;
}
inline ObjectObj *Value::get_obj_ptr() {
    auto *p = heap_ptr();
    return p && p->type_tag == ValueTag::Object ? ((ObjectObj *)p) : nullptr;
}
inline const ObjectObj *Value::get_obj_ptr() const {
    auto *p = heap_ptr();
    return p && p->type_tag == ValueTag::Object ? ((const ObjectObj *)p) : nullptr;
}
inline FunctionData &Value::get_function() {
    return *(FunctionData *)heap_ptr();
}
inline const FunctionData &Value::get_function() const {
    return *(const FunctionData *)heap_ptr();
}
inline HandlePtr Value::get_handle() const {
    return is_handle() ? (HandleData *)heap_ptr() : nullptr;
}
inline ClassInstancePtr Value::get_class_instance() const {
    return is_class_instance() ? (ClassInstance *)heap_ptr() : nullptr;
}
inline RegexObj *Value::get_regex() {
    return is_regex() ? (RegexObj *)heap_ptr() : nullptr;
}
inline const RegexObj *Value::get_regex() const {
    return is_regex() ? (const RegexObj *)heap_ptr() : nullptr;
}
inline DelegateData *Value::get_delegate() const {
    return is_delegate() ? (DelegateData *)heap_ptr() : nullptr;
}
inline void Value::set_int(int64_t v) {
    *this = make_int(v);
}
inline void Value::set_int_checked(int64_t v) {
    *this = make_int_checked(v);
}
inline void Value::set_bool(bool v) {
    *this = make_bool(v);
}
inline void Value::set_float(double v) {
    *this = make_float(v);
}
inline size_t Value::tag() const {
    if (is_none()) {
        return 0;
    }
    if (is_int()) {
        return 1;
    }
    if (is_float()) {
        return 2;
    }
    if (is_bool()) {
        return 3;
    }
    return (size_t)heap_tag();
}
inline double Value::as_number() const {
    if (is_int()) {
        return (double)get_int();
    }
    if (is_float()) {
        return get_float();
    }
    if (is_bool()) {
        return get_bool() ? 1.0 : 0.0;
    }
    if (is_string()) {
        const auto &s = get_string();
        if (s.empty()) {
            return 0.0;
        }
        char *end = nullptr;
        double d = std::strtod(s.c_str(), &end);
        return end && *end == '\0' ? d : 0.0;
    }
    if (is_array()) {
        return (double)get_array().size();
    }
    if (is_object()) {
        const auto *o = get_obj_ptr();
        return o ? (double)o->field_count() : 0.0;
    }
    return 0.0;
}
inline bool Value::as_bool() const {
    if (is_none()) {
        return false;
    }
    if (is_bool()) {
        return get_bool();
    }
    if (is_int()) {
        return get_int() != 0;
    }
    if (is_float()) {
        return get_float() != 0.0;
    }
    if (is_string()) {
        return !get_string().empty();
    }
    if (is_array()) {
        return !get_array().empty();
    }
    if (is_object()) {
        const auto *o = get_obj_ptr();
        return o && !o->is_empty();
    }
    return heap_ptr() != nullptr;
}
inline bool is_truthy(const Value &v) {
    return v.as_bool();
}
inline bool is_js_truthy(const Value &v) {
    if (v.is_none()) {
        return false;
    }
    if (v.is_bool()) {
        return v.get_bool();
    }
    if (v.is_int()) {
        return v.get_int() != 0;
    }
    if (v.is_float()) {
        double value = v.get_float();
        return value != 0.0 && !std::isnan(value);
    }
    if (v.is_string()) {
        return !v.get_string().empty();
    }
    return true;
}
inline std::string Value::to_string(bool in_container_context) const {
    // `seen` tracks the ancestor container chain to prevent recursing until the C stack overflows.
    std::vector<const void *> seen;
    return to_string_impl(in_container_context, seen);
}
inline std::string Value::to_string_impl(bool in_container_context, std::vector<const void *> &seen) const {
    constexpr size_t kMaxDepth = 128; // also caps non-cyclic nesting
    if (is_none()) {
        return "<null>";
    }
    if (is_int()) {
        return std::to_string(get_int());
    }
    if (is_float()) {
        char buf[64];
        auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), get_float());
        return ec == std::errc() ? std::string(buf, p) : std::to_string(get_float());
    }
    if (is_bool()) {
        return get_bool() ? "true" : "false";
    }
    if (is_string()) {
        const auto &s = get_string();
        return in_container_context ? (std::string("\"") + s + "\"") : s;
    }
    if (is_array()) {
        const void *p = heap_ptr();
        for (const void *q : seen) {
            if (q == p) {
                return "[Circular]";
            }
        }
        if (seen.size() >= kMaxDepth) {
            return "...";
        }
        seen.push_back(p);
        std::string r = "[";
        const auto &a = get_array();
        for (size_t i = 0; i < a.size(); i++) {
            if (i) {
                r += ", ";
            }
            r += a[i].to_string_impl(true, seen);
        }
        r += "]";
        seen.pop_back();
        return r;
    }
    if (is_object()) {
        const void *p = heap_ptr();
        for (const void *q : seen) {
            if (q == p) {
                return "[Circular]";
            }
        }
        if (seen.size() >= kMaxDepth) {
            return "...";
        }
        seen.push_back(p);
        std::string r = "{";
        const auto *o = get_obj_ptr();
        bool first = true;
        if (o) {
            for (const auto &k : o->get_keys()) {
                const Value *v = o->get_field(k);
                if (!first) {
                    r += ", ";
                }
                first = false;
                r += k + ": " + (v ? v->to_string_impl(true, seen) : Value::none().to_string_impl(true, seen));
            }
        }
        r += "}";
        seen.pop_back();
        return r;
    }
    if (is_function()) {
        return "<function " + get_function().name + ">";
    }
    if (is_handle()) {
        return "<handle>";
    }
    if (is_class_instance()) {
        auto *ci = get_class_instance();
        return ci ? ("<" + ci->class_name + " instance>") : "<instance>";
    }
    if (is_regex()) {
        const auto *re = get_regex();
        return re ? ("/" + re->pattern + "/" + re->flags) : "/(?:)/";
    }
    if (is_delegate()) {
        const void *p = heap_ptr();
        for (const void *q : seen) {
            if (q == p) {
                return "[Circular]";
            }
        }
        if (seen.size() >= kMaxDepth) {
            return "...";
        }
        auto *d = get_delegate();
        if (!d) {
            return "<delegate>";
        }
        seen.push_back(p);
        std::string r = "<delegate " + d->target.to_string_impl(true, seen) + ">";
        seen.pop_back();
        return r;
    }
    return "<unknown>";
}
inline bool Value::values_equal(const Value &a, const Value &b) {
    if (a.is_none() || b.is_none()) {
        return a.is_none() && b.is_none();
    }
    if (a.is_numeric() && b.is_numeric()) {
        return std::fabs(a.as_number() - b.as_number()) < 1e-12;
    }
    if (a.is_bool() || b.is_bool()) {
        return a.is_bool() && b.is_bool() && a.get_bool() == b.get_bool();
    }
    if (a.is_string() || b.is_string()) {
        return a.is_string() && b.is_string() && a.get_string() == b.get_string();
    }
    return a._raw == b._raw;
}

// Three-way ordering, shared by <, <=, > and >=. The bytecode interpreter and the
// JIT helpers both reach the operators through this one function.
//
// Returns false when a and b have no order, and then every relational operator
// gives false. Only two values of the same class have an order. This keeps the
// operators coherent with values_equal(): if two values are ==, they are also <=
// and >=, and a value that is < another is never == to it.
//
// Numbers use the same 1e-12 tolerance as values_equal(). Without it, a pair that
// == calls equal could still report < or >.
//
// The branch order matches values_equal(). is_numeric() covers int and float only,
// so a bool never takes the numeric path.
inline bool Value::compare_ordered(const Value &a, const Value &b, int &cmp) {
    if (a.is_none() || b.is_none()) {
        if (!a.is_none() || !b.is_none()) {
            return false;
        }
        cmp = 0;
        return true;
    }
    if (a.is_int() && b.is_int()) {
        const int64_t x = a.get_int();
        const int64_t y = b.get_int();
        cmp = x < y ? -1 : (x > y ? 1 : 0);
        return true;
    }
    if (a.is_numeric() && b.is_numeric()) {
        const double x = a.as_number();
        const double y = b.as_number();
        cmp = std::fabs(x - y) < 1e-12 ? 0 : (x < y ? -1 : 1);
        return true;
    }
    if (a.is_bool() || b.is_bool()) {
        if (!a.is_bool() || !b.is_bool()) {
            return false;
        }
        const int x = a.get_bool() ? 1 : 0;
        const int y = b.get_bool() ? 1 : 0;
        cmp = x < y ? -1 : (x > y ? 1 : 0);
        return true;
    }
    if (a.is_string() || b.is_string()) {
        if (!a.is_string() || !b.is_string()) {
            return false;
        }
        const std::string &x = a.get_string();
        const std::string &y = b.get_string();
        cmp = x < y ? -1 : (x > y ? 1 : 0);
        return true;
    }
    // Two heap values have an order only when they are the same object.
    if (a._raw == b._raw) {
        cmp = 0;
        return true;
    }
    return false;
}

inline bool Value::values_lt(const Value &a, const Value &b) {
    int cmp = 0;
    return compare_ordered(a, b, cmp) && cmp < 0;
}

inline bool Value::values_le(const Value &a, const Value &b) {
    int cmp = 0;
    return compare_ordered(a, b, cmp) && cmp <= 0;
}

inline bool Value::values_gt(const Value &a, const Value &b) {
    int cmp = 0;
    return compare_ordered(a, b, cmp) && cmp > 0;
}

inline bool Value::values_ge(const Value &a, const Value &b) {
    int cmp = 0;
    return compare_ordered(a, b, cmp) && cmp >= 0;
}

inline bool Value::values_strict_equal(const Value &a, const Value &b) {
    const uint16_t a_tag = a.tag_word();
    const uint16_t b_tag = b.tag_word();

    if (a_tag == TAG_HEAP && b_tag == TAG_HEAP) {
        if (a._raw == b._raw) {
            return true;
        }
        HeapHeader *a_heap = reinterpret_cast<HeapHeader *>(a._raw & PTR_MASK);
        HeapHeader *b_heap = reinterpret_cast<HeapHeader *>(b._raw & PTR_MASK);
        return a_heap->type_tag == ValueTag::String && b_heap->type_tag == ValueTag::String &&
               static_cast<StringObj *>(a_heap)->s == static_cast<StringObj *>(b_heap)->s;
    }
    if (a_tag == TAG_INT && b_tag == TAG_INT) {
        return a._raw == b._raw;
    }
    if (a.is_numeric() && b.is_numeric()) {
        return a.as_number() == b.as_number();
    }
    return a_tag == b_tag && a._raw == b._raw;
}
