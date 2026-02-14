#include "core_types.h"
#include "gc.h"

Value Value::make_array() {
    Value val;
    auto arr = std::make_shared<std::vector<Value>>();
    GarbageCollector::instance().track(arr);
    val.data = arr;
    return val;
}

Value Value::make_array(std::vector<Value> elements) {
    Value val;
    auto arr = std::make_shared<std::vector<Value>>(std::move(elements));
    GarbageCollector::instance().track(arr);
    val.data = arr;
    return val;
}

Value Value::make_object() {
    Value val;
    auto obj = std::make_shared<std::unordered_map<std::string, Value>>();
    GarbageCollector::instance().track(obj);
    val.data = obj;
    return val;
}

Value Value::make_object(Object entries) {
    Value val;
    if (entries) {
        GarbageCollector::instance().track(entries);
    }
    val.data = std::move(entries);
    return val;
}

HandlePtr Value::make_handle_ptr() {
    auto handle = std::make_shared<HandleData>();
    GarbageCollector::instance().track(handle);
    return handle;
}

Value Value::make_handle(HandlePtr handlePtr) {
    Value val;
    if (handlePtr) {
        GarbageCollector::instance().track(handlePtr);
    }
    val.data = std::move(handlePtr);
    return val;
}
