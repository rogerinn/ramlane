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
    LayoutEngine() = default;

    void load_layout_json(const std::string& path);
    void load_map_flatbuf(const std::string& path);
    void save_map_flatbuf(const std::string& path);

    void allocate_memory_from_file(const std::string& path);
    void* mmap_base();
    size_t mmap_size() const;

    void generate_ffi_header(const std::string& output_path);
    void generate_ffi_cpp(const std::string& output_path);

    void insert(const std::string& field, const void* item);
    void pop(const std::string& field, size_t index);
    void* get(const std::string& field, size_t index = 0);

private:
    void build_layout(const nlohmann::json& layout);

    LayoutMap map_;
    void* base_ptr_ = nullptr;
    size_t size_ = 0;
};
