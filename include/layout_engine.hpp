#pragma once

#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

enum class FieldType { Int32, Int64, Float32, Float64, String, Object, Array };

struct FieldLayout {
  std::string name;
  FieldType type;
  size_t offset = 0;
  size_t size = 0;
  size_t max_length = 0;      // para string
  size_t count_offset = 0;    // para array
  size_t item_stride = 0;     // para array
  size_t max_items = 0;       // para array
  bool has_used_flag = false; // para array
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

  // Geração de mapa
  void load_layout_json(const std::string &path);
  void build_layout(const nlohmann::json &layout_def);
  void save_map_flatbuf(const std::string &path);
  void load_map_flatbuf(const std::string &path);

  // Alocação de memória
  void allocate_memory_from_file(const std::string &path);
  void *mmap_base() const;
  size_t mmap_size() const;

  // Operações de inserção/pop/get (internas)
  void insert(const std::string &field_name, const void *item);
  void pop(const std::string &field_name, size_t index);
  void *get(const std::string &field_name, size_t index = 0);

  // Geração de FFI (header + source)
  void generate_ffi_header(const std::string &output_path);
  void generate_ffi_cpp(const std::string &output_path);

  void validate_and_format(const std::string &header_path,
    const std::string &cpp_path);

private:
  LayoutMap map_;
  void *base_ptr_ = nullptr;
  size_t size_ = 0;
};
