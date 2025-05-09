#include "layout_engine.hpp"
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <iostream>

using json = nlohmann::json;

LayoutEngine::LayoutEngine(const json& layout) {
    build_layout(layout["layout"]);
    allocate_memory();  // pode ser substituído por allocate_memory_from_file()
}

void LayoutEngine::build_layout(const json& layout_def) {
    size_t offset = 0;
    for (auto it = layout_def.begin(); it != layout_def.end(); ++it) {
        FieldLayout field;
        field.name = it.key();
        const auto& def = it.value();

        std::string type = def["type"];
        if (type == "int32") field.type = FieldType::Int32, field.size = 4;
        else if (type == "int64") field.type = FieldType::Int64, field.size = 8;
        else if (type == "float32") field.type = FieldType::Float32, field.size = 4;
        else if (type == "float64") field.type = FieldType::Float64, field.size = 8;
        else if (type == "string") {
            field.type = FieldType::String;
            field.max_length = def.value("max_length", 256);
            field.size = field.max_length;
        }
        else if (type == "object") {
            field.type = FieldType::Object;
            size_t inner_offset = 0;
            for (auto& [key, val] : def["schema"].items()) {
                FieldLayout child;
                child.name = key;
                std::string t = val;
                if (t == "int32") child.type = FieldType::Int32, child.size = 4;
                else if (t == "float32") child.type = FieldType::Float32, child.size = 4;
                else if (t == "float64") child.type = FieldType::Float64, child.size = 8;
                child.offset = inner_offset;
                field.field_index[key] = field.children.size();
                field.children.push_back(child);
                inner_offset += child.size;
            }
            field.size = inner_offset;
        }
        else if (type == "object[]") {
            field.type = FieldType::Array;
            field.max_items = def["max_items"];
            field.count_offset = offset;
            offset += 4;
            field.has_used_flag = true;

            size_t item_offset = 0;
            for (auto& [key, val] : def["schema"].items()) {
                FieldLayout child;
                child.name = key;
                std::string t = val;
                if (t == "int32") child.type = FieldType::Int32, child.size = 4;
                else if (t == "float32") child.type = FieldType::Float32, child.size = 4;
                else if (t == "float64") child.type = FieldType::Float64, child.size = 8;
                child.offset = item_offset;
                field.field_index[key] = field.children.size();
                field.children.push_back(child);
                item_offset += child.size;
            }

            field.item_stride = item_offset + (field.has_used_flag ? 1 : 0);
            field.size = field.item_stride * field.max_items;
        }

        field.offset = offset;
        map_.field_index[field.name] = map_.fields.size();
        map_.fields.push_back(field);
        offset += field.size;
    }

    map_.total_size = offset;
}

void LayoutEngine::allocate_memory() {
    size_ = map_.total_size;
    int fd = memfd_create("layout_buffer", 0);
    if (fd < 0) throw std::runtime_error("memfd_create failed");

    if (ftruncate(fd, size_) < 0) {
        close(fd);
        throw std::runtime_error("ftruncate failed");
    }

    base_ptr_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base_ptr_ == MAP_FAILED) {
        close(fd);
        throw std::runtime_error("mmap failed");
    }

    close(fd);
}

void LayoutEngine::allocate_memory_from_file(const std::string& path) {
    size_ = map_.total_size;
    int fd = open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) throw std::runtime_error("open(tmpfs path) failed");

    if (ftruncate(fd, size_) < 0) {
        close(fd);
        throw std::runtime_error("ftruncate failed");
    }

    base_ptr_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base_ptr_ == MAP_FAILED) {
        close(fd);
        throw std::runtime_error("mmap failed");
    }

    close(fd);
}

void* LayoutEngine::mmap_base() {
    return base_ptr_;
}

size_t LayoutEngine::mmap_size() const {
    return size_;
}

void LayoutEngine::insert(const std::string& field_name, const void* item) {
    const auto& field = map_.fields[map_.field_index.at(field_name)];
    if (field.type != FieldType::Array)
        throw std::runtime_error("insert only supports array fields");

    uint32_t* count = reinterpret_cast<uint32_t*>((char*)base_ptr_ + field.count_offset);
    if (*count >= field.max_items)
        throw std::runtime_error("array full");

    size_t base = field.offset + 4 + (*count * field.item_stride);
    if (field.has_used_flag) {
        *((char*)base_ptr_ + base) = 1;
        ++base;
    }

    std::memcpy((char*)base_ptr_ + base, item, field.item_stride - (field.has_used_flag ? 1 : 0));
    (*count)++;
}

void LayoutEngine::pop(const std::string& field_name, size_t index) {
    const auto& field = map_.fields[map_.field_index.at(field_name)];
    if (field.type != FieldType::Array)
        throw std::runtime_error("pop only supports array fields");

    uint32_t* count = reinterpret_cast<uint32_t*>((char*)base_ptr_ + field.count_offset);
    if (index >= *count)
        throw std::runtime_error("index out of bounds");

    size_t base = field.offset + 4 + (index * field.item_stride);
    if (field.has_used_flag) {
        *((char*)base_ptr_ + base) = 0;
    }
}

void* LayoutEngine::get(const std::string& field_name, size_t index) {
    const auto& field = map_.fields[map_.field_index.at(field_name)];

    if (field.type == FieldType::Array) {
        uint32_t* count = reinterpret_cast<uint32_t*>((char*)base_ptr_ + field.count_offset);
        if (index >= *count) return nullptr;

        size_t base = field.offset + 4 + (index * field.item_stride);
        if (field.has_used_flag && *((char*)base_ptr_ + base) == 0) return nullptr;

        return (char*)base_ptr_ + base + (field.has_used_flag ? 1 : 0);
    } else {
        if (index > 0) return nullptr;
        return (char*)base_ptr_ + field.offset;
    }
}
