#pragma once

#include <memory>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct Value;
struct HandleData;

// mark and sweep GC
class GarbageCollector {
public:
  static GarbageCollector &instance() {
    static GarbageCollector gc;
    return gc;
  }

  // type tag for tracked objects (breaks cycles during sweep)
  enum class TrackedType { Array, Object, Handle, Unknown };

  struct TrackedEntry {
    std::weak_ptr<void> ptr;
    TrackedType type;
  };

  // allocation tracking
  template <typename T> std::shared_ptr<T> track(std::shared_ptr<T> ptr) {
    if (!ptr || !enabled)
      return ptr;

    std::lock_guard<std::mutex> lock(mutex);

    void *key = static_cast<void *>(ptr.get());

    // Determine tracked type for cycle-breaking during sweep
    TrackedType type = TrackedType::Unknown;
    if constexpr (std::is_same_v<T, std::vector<Value>>) {
      type = TrackedType::Array;
    } else if constexpr (std::is_same_v<
                             T, std::unordered_map<std::string, Value>>) {
      type = TrackedType::Object;
    } else if constexpr (std::is_same_v<T, HandleData>) {
      type = TrackedType::Handle;
    }

    tracked_objects[key] = TrackedEntry{std::weak_ptr<void>(ptr), type};

    allocation_count++;
    total_allocated++;
    
    // Estimate memory usage (rough approximation)
    // Value is roughly 64 bytes (variant overhead + largest member)
    // Each tracked object: ~40 bytes overhead + allocation size
    constexpr size_t VALUE_SIZE_ESTIMATE = 64;
    if constexpr (std::is_same_v<T, std::vector<Value>>) {
      estimated_memory_usage += 40 + (ptr->capacity() * VALUE_SIZE_ESTIMATE);
    } else if constexpr (std::is_same_v<T, std::unordered_map<std::string, Value>>) {
      estimated_memory_usage += 56 + (ptr->size() * (32 + VALUE_SIZE_ESTIMATE + 24));
    } else {
      estimated_memory_usage += 40 + sizeof(T);
    }

    // update peak tracking
    if (tracked_objects.size() > peak_tracked) {
      peak_tracked = tracked_objects.size();
    }
    
    // Check memory limit and force collection if exceeded
    if (memory_limit > 0 && estimated_memory_usage > memory_limit) {
      trigger_collection = true;
    }

    // prune expired entries more aggressively for low-memory targets
    if (allocation_count % (collection_threshold / 2) == 0) {
      prune_expired_locked();
    }

    // check if we need to trigger a collection
    if (allocation_count >= collection_threshold) {
      trigger_collection = true;
    }

    return ptr;
  }

  // mark all reachable objects
  void mark_value(const Value &v);

  // sweep unmarked objects
  size_t sweep();

  // full collection
  size_t collect(const std::vector<const Value *> &roots);

  // some simple statistics, currently the script can call these to get
  // information but i'm not sure if I want to keep it like that, since a script
  // knowing about gc internals could be bad
  size_t get_tracked_count() const {
    std::lock_guard<std::mutex> lock(mutex);
    return tracked_objects.size();
  }
  size_t get_allocation_count() const { return this->allocation_count; }
  size_t get_total_collections() const { return this->total_collections; }
  size_t get_total_collected() const { return this->total_collected; }
  size_t get_total_allocated() const { return this->total_allocated; }
  size_t get_peak_tracked() const { return this->peak_tracked; }

  // force immediate collection, and returns number collected
  size_t force_collect(const std::vector<const Value *> &roots);

  // detailed statistics
  struct Stats {
    size_t tracked_count;
    size_t allocation_count;
    size_t total_collections;
    size_t total_collected;
    size_t total_allocated;
    size_t peak_tracked;
    size_t collection_threshold;
    size_t refcount_freed;
    bool enabled;
  };

  Stats get_stats() const;

  void reset_allocation_count() {
    this->allocation_count = 0;
    this->trigger_collection = false;
  }

  bool should_collect() const { return this->trigger_collection; }

  // gc configuration
  void set_collection_threshold(size_t threshold) {
    this->collection_threshold = threshold;
  }

  size_t get_collection_threshold() const { return collection_threshold; }

  void set_enabled(bool enabled) { this->enabled = enabled; }

  bool is_enabled() const { return this->enabled; }
  
  // Set memory limit in bytes (0 = unlimited)
  void set_memory_limit(size_t limit) { this->memory_limit = limit; }
  
  size_t get_memory_limit() const { return memory_limit; }
  
  size_t get_estimated_memory_usage() const { return estimated_memory_usage; }

  // mark a raw ptr as reachable
  void mark_ptr(void *ptr);

  // check if a raw ptr is marked
  bool is_marked(void *ptr) const;

  // clear marks after sweep
  void clear_marks();

private:
  GarbageCollector() = default;
  ~GarbageCollector() = default;
  GarbageCollector(const GarbageCollector &) = delete;
  GarbageCollector &operator=(const GarbageCollector &) = delete;

  mutable std::mutex mutex;

  // all allocated objects <raw_pointer, weak_ptr + type>
  std::unordered_map<void *, TrackedEntry> tracked_objects;

  // set of marked objects for current GC cycle
  std::unordered_set<void *> marked_objects;

  size_t allocation_count = 0;
  size_t collection_threshold = 100;  // Reduced from 1000 for lower memory footprint
  bool trigger_collection = false;
  bool enabled = true;

  size_t total_collections = 0;
  size_t total_collected = 0;
  size_t total_allocated = 0;
  size_t peak_tracked = 0;
  size_t refcount_freed = 0;
  
  // Memory limit (0 = unlimited)
  size_t memory_limit = 0;
  size_t estimated_memory_usage = 0;

  // prune expired entries from the tracking map
  // you must have the mutex to call this or bad things will happen
  void prune_expired_locked();

  // Mark helpers for different types
  void mark_array(const std::shared_ptr<std::vector<Value>> &arr);
  void mark_object(
      const std::shared_ptr<std::unordered_map<std::string, Value>> &obj);
  void mark_handle(const std::shared_ptr<HandleData> &handle);
};