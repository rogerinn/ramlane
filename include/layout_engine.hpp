#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <nlohmann/json.hpp>

enum class FieldType {
    Int32, Int64, Float32, Float64, String, Object, Array
};

struct FieldLayout {
    std::string name;
    FieldType type;
    size_t offset = 0;
    size_t size = 0;

    size_t max_length = 0;
    size_t count_offset = 0;
    size_t max_items = 0;
    size_t item_stride = 0;
    bool has_used_flag = false;

    std::vector<FieldLayout> children;
    std::unordered_map<std::string, size_t> field_index;
};

struct LayoutMap {
    size_t total_size = 0;
    std::vector<FieldLayout> fields;
    std::unordered_map<std::string, size_t> field_index;
};

class LayoutEngine {
public:
    LayoutEngine(const nlohmann::json& layout);

    void* mmap_base();
    size_t mmap_size() const;

    void insert(const std::string& field, const void* item);
    void pop(const std::string& field, size_t index);
    void* get(const std::string& field, size_t index = 0);

    void allocate_memory_from_file(const std::string& path);

private:
    LayoutMap map_;
    void* base_ptr_ = nullptr;
    size_t size_ = 0;

    void build_layout(const nlohmann::json& layout);
    void allocate_memory();
};
