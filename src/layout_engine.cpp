#include "layout_engine.hpp"
#include "layout_map_generated.h"

#include <cstring>
#include <fcntl.h>
#include <flatbuffers/flatbuffers.h>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

using json = nlohmann::json;

void LayoutEngine::build_layout(const json &layout_def) {
  size_t offset = 0;
  for (auto it = layout_def.begin(); it != layout_def.end(); ++it) {
    FieldLayout field;
    field.name = it.key();
    const auto &def = it.value();

    std::string type = def["type"];
    if (type == "int32")
      field.type = FieldType::Int32, field.size = 4;
    else if (type == "int64")
      field.type = FieldType::Int64, field.size = 8;
    else if (type == "float32")
      field.type = FieldType::Float32, field.size = 4;
    else if (type == "float64")
      field.type = FieldType::Float64, field.size = 8;
    else if (type == "string") {
      field.type = FieldType::String;
      field.max_length = def.value("max_length", 256);
      field.size = field.max_length;
    } else if (type == "object") {
      field.type = FieldType::Object;
      size_t inner_offset = 0;
      for (auto &[key, val] : def["schema"].items()) {
        FieldLayout child;
        child.name = key;
        std::string t = val;
        if (t == "int32")
          child.type = FieldType::Int32, child.size = 4;
        else if (t == "float32")
          child.type = FieldType::Float32, child.size = 4;
        else if (t == "float64")
          child.type = FieldType::Float64, child.size = 8;
        child.offset = inner_offset;
        field.field_index[key] = field.children.size();
        field.children.push_back(child);
        inner_offset += child.size;
      }
      field.size = inner_offset;
    } else if (type == "object[]") {
      field.type = FieldType::Array;
      field.max_items = def["max_items"];
      field.count_offset = offset;
      offset += 4;
      field.has_used_flag = true;

      size_t item_offset = 0;
      for (auto &[key, val] : def["schema"].items()) {
        FieldLayout child;
        child.name = key;
        std::string t = val;
        if (t == "int32")
          child.type = FieldType::Int32, child.size = 4;
        else if (t == "float32")
          child.type = FieldType::Float32, child.size = 4;
        else if (t == "float64")
          child.type = FieldType::Float64, child.size = 8;
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

void LayoutEngine::allocate_memory_from_file(const std::string &path) {
  size_ = map_.total_size;
  int fd = open(path.c_str(), O_RDWR | O_CREAT, 0600);
  if (fd < 0)
    throw std::runtime_error("open(tmpfs path) failed");

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

void *LayoutEngine::mmap_base() { return base_ptr_; }

size_t LayoutEngine::mmap_size() const { return size_; }

void LayoutEngine::insert(const std::string &field_name, const void *item) {
  const auto &field = map_.fields[map_.field_index.at(field_name)];
  if (field.type != FieldType::Array)
    throw std::runtime_error("insert only supports array fields");

  uint32_t *count =
      reinterpret_cast<uint32_t *>((char *)base_ptr_ + field.count_offset);
  if (*count >= field.max_items)
    throw std::runtime_error("array full");

  size_t base = field.offset + 4 + (*count * field.item_stride);
  if (field.has_used_flag) {
    *((char *)base_ptr_ + base) = 1;
    ++base;
  }

  std::memcpy((char *)base_ptr_ + base, item,
              field.item_stride - (field.has_used_flag ? 1 : 0));
  (*count)++;
}

void LayoutEngine::pop(const std::string &field_name, size_t index) {
  const auto &field = map_.fields[map_.field_index.at(field_name)];
  if (field.type != FieldType::Array)
    throw std::runtime_error("pop only supports array fields");

  uint32_t *count =
      reinterpret_cast<uint32_t *>((char *)base_ptr_ + field.count_offset);
  if (index >= *count)
    throw std::runtime_error("index out of bounds");

  size_t base = field.offset + 4 + (index * field.item_stride);
  if (field.has_used_flag) {
    *((char *)base_ptr_ + base) = 0;
  }
}

void *LayoutEngine::get(const std::string &field_name, size_t index) {
  const auto &field = map_.fields[map_.field_index.at(field_name)];

  if (field.type == FieldType::Array) {
    uint32_t *count =
        reinterpret_cast<uint32_t *>((char *)base_ptr_ + field.count_offset);
    if (index >= *count)
      return nullptr;

    size_t base = field.offset + 4 + (index * field.item_stride);
    if (field.has_used_flag && *((char *)base_ptr_ + base) == 0)
      return nullptr;

    return (char *)base_ptr_ + base + (field.has_used_flag ? 1 : 0);
  } else {
    if (index > 0)
      return nullptr;
    return (char *)base_ptr_ + field.offset;
  }
}

void LayoutEngine::save_map_flatbuf(const std::string &path) {
  flatbuffers::FlatBufferBuilder builder(1024);

  std::function<flatbuffers::Offset<Layout::Field>(const FieldLayout &)>
      build_field;
  build_field =
      [&](const FieldLayout &f) -> flatbuffers::Offset<Layout::Field> {
    std::vector<flatbuffers::Offset<Layout::Field>> children;
    for (const auto &c : f.children) {
      children.push_back(build_field(c));
    }

    return Layout::CreateField(builder, builder.CreateString(f.name),
                               static_cast<Layout::FieldType>(f.type), f.offset,
                               f.size, f.count_offset, f.item_stride,
                               f.max_items, f.has_used_flag,
                               builder.CreateVector(children));
  };

  std::vector<flatbuffers::Offset<Layout::Field>> fields;
  for (const auto &f : map_.fields) {
    fields.push_back(build_field(f));
  }

  auto layout = Layout::CreateLayoutMap(builder, map_.total_size,
                                        builder.CreateVector(fields));
  builder.Finish(layout);

  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char *>(builder.GetBufferPointer()),
            builder.GetSize());
}

void LayoutEngine::load_map_flatbuf(const std::string &path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    throw std::runtime_error("Cannot open map file");
  std::streamsize size = in.tellg();
  in.seekg(0, std::ios::beg);

  std::vector<char> buffer(size);
  if (!in.read(buffer.data(), size))
    throw std::runtime_error("Failed to read map file");

  const auto *layout = Layout::GetLayoutMap(buffer.data());

  map_.total_size = layout->total_size();
  map_.fields.clear();
  map_.field_index.clear();

  std::function<FieldLayout(const Layout::Field *)> parse_field;
  parse_field = [&](const Layout::Field *f) -> FieldLayout {
    FieldLayout field;
    field.name = f->name()->str();
    field.type = static_cast<FieldType>(f->type());
    field.offset = f->offset();
    field.size = f->size();
    field.count_offset = f->count_offset();
    field.item_stride = f->stride();
    field.max_items = f->max_items();
    field.has_used_flag = f->has_used_flag();

    if (f->children()) {
      for (const auto *c : *f->children()) {
        auto child = parse_field(c);
        field.field_index[child.name] = field.children.size();
        field.children.push_back(std::move(child));
      }
    }
    return field;
  };

  for (const auto *f : *layout->fields()) {
    auto field = parse_field(f);
    map_.field_index[field.name] = map_.fields.size();
    map_.fields.push_back(std::move(field));
  }
}

void LayoutEngine::generate_ffi_header(const std::string &output_path) {
  std::ofstream out(output_path);
  out << "#pragma once\n#include <cstddef>\n\n";

  // Sub-structs (object e object[])
  for (const auto &field : map_.fields) {
    if (field.type == FieldType::Object) {
      out << "struct " << field.name << " {\n";
      for (const auto &child : field.children) {
        if (child.type == FieldType::Int32)
          out << "    int " << child.name << ";\n";
        else if (child.type == FieldType::Float32)
          out << "    float " << child.name << ";\n";
        else if (child.type == FieldType::Float64)
          out << "    double " << child.name << ";\n";
      }
      out << "};\n\n";
    }
    if (field.type == FieldType::Array) {
      out << "struct " << field.name << " {\n";
      for (const auto &child : field.children) {
        if (child.type == FieldType::Int32)
          out << "    int " << child.name << ";\n";
        else if (child.type == FieldType::Float32)
          out << "    float " << child.name << ";\n";
        else if (child.type == FieldType::Float64)
          out << "    double " << child.name << ";\n";
      }
      out << "};\n\n";
    }
  }

  // Struct root_layout
  out << "struct root_layout {\n";
  for (const auto &field : map_.fields) {
    if (field.type == FieldType::Int32)
      out << "    int " << field.name << ";\n";
    else if (field.type == FieldType::Float32)
      out << "    float " << field.name << ";\n";
    else if (field.type == FieldType::Float64)
      out << "    double " << field.name << ";\n";
    else if (field.type == FieldType::String)
      out << "    char " << field.name << "[" << field.max_length << "];\n";
    else if (field.type == FieldType::Object)
      out << "    struct " << field.name << " " << field.name << ";\n";
    else if (field.type == FieldType::Array)
      out << "    struct " << field.name << " " << field.name << "["
          << field.max_items << "];\n";
  }
  out << "};\n\n";

  out << "extern \"C\" {\n";

  for (const auto &field : map_.fields) {
    if (field.type == FieldType::Int32) {
      out << "int get_" << field.name << "();\n";
      out << "void set_" << field.name << "(int value);\n";
    } else if (field.type == FieldType::Float64) {
      out << "double get_" << field.name << "();\n";
      out << "void set_" << field.name << "(double value);\n";
    } else if (field.type == FieldType::Float32) {
      out << "float get_" << field.name << "();\n";
      out << "void set_" << field.name << "(float value);\n";
    } else if (field.type == FieldType::String) {
      out << "const char* get_" << field.name << "();\n";
      out << "void set_" << field.name << "(const char* value);\n";
    } else if (field.type == FieldType::Object) {
      for (const auto &child : field.children) {
        const std::string full = field.name + "_" + child.name;
        if (child.type == FieldType::Int32)
          out << "int get_" << full << "();\nvoid set_" << full << "(int);\n";
        if (child.type == FieldType::Float32)
          out << "float get_" << full << "();\nvoid set_" << full
              << "(float);\n";
        if (child.type == FieldType::Float64)
          out << "double get_" << full << "();\nvoid set_" << full
              << "(double);\n";
      }
    } else if (field.type == FieldType::Array) {
      out << "std::size_t get_" << field.name << "_count();\n";
      out << "void set_" << field.name << "_count(std::size_t count);\n";

      for (const auto &child : field.children) {
        const std::string full = field.name + "_" + child.name;
        std::string type_str;
        if (child.type == FieldType::Float64)
          type_str = "double";
        else if (child.type == FieldType::Float32)
          type_str = "float";
        else if (child.type == FieldType::Int32)
          type_str = "int";
        else
          continue;

        out << type_str << " get_" << full << "(std::size_t index);\n";
        out << "void set_" << full << "(std::size_t index, " << type_str
            << " value);\n";
      }
    }
  }

  out << "}\n";
}

void LayoutEngine::generate_ffi_cpp(const std::string &output_path) {
  std::string header_path =
      output_path.substr(0, output_path.find_last_of('.')) + ".hpp";
  std::ifstream header(header_path);
  if (!header)
    throw std::runtime_error("Header FFI não encontrado: " + header_path);

  std::vector<std::string> declarations;
  std::string line;
  while (std::getline(header, line)) {
    if (line.find("get_") != std::string::npos ||
        line.find("set_") != std::string::npos)
      declarations.push_back(line);
  }

  std::ofstream out(output_path);
  out << "#include <cstddef>\n#include <cstdint>\n#include <cstring>\n\nextern "
         "void* base_ptr;\n\n";

  for (const auto &field : map_.fields) {
    out << "// " << field.name << "\n";
    if (field.type == FieldType::String)
      out << "constexpr std::size_t " << field.name
          << "_MAX_LEN = " << field.max_length << ";\n";

    if (field.type == FieldType::Array) {
      out << "constexpr std::size_t OFFSET_" << field.name
          << "_count = " << field.count_offset << ";\n";
      out << "constexpr std::size_t OFFSET_" << field.name
          << "_base = " << field.offset + 4 << ";\n";
      out << "constexpr std::size_t STRIDE_" << field.name << " = "
          << field.item_stride << ";\n";
      for (const auto &child : field.children) {
        out << "constexpr std::size_t OFFSET_" << field.name << "_"
            << child.name << " = "
            << child.offset + (field.has_used_flag ? 1 : 0) << ";\n";
      }
    } else if (field.type == FieldType::Object) {
      for (const auto &child : field.children) {
        out << "constexpr std::size_t OFFSET_" << field.name << "_"
            << child.name << " = " << field.offset + child.offset << ";\n";
      }
    } else {
      out << "constexpr std::size_t OFFSET_" << field.name << " = "
          << field.offset << ";\n";
    }
    out << "\n";
  }

  for (const auto &decl : declarations) {
    std::smatch match;
    if (std::regex_match(
            decl, match,
            std::regex(
                R"((int|float|double|const char\*) get_(\w+)(?:\((std::size_t index)?\))?;)"))) {
      std::string rettype = match[1];
      std::string name = match[2];
      bool is_array = decl.find("std::size_t index") != std::string::npos;

      // Detectar nome pai se for array
      std::string parent = name;
      if (is_array && name.find('_') != std::string::npos) {
        parent = name.substr(0, name.find('_'));
      }

      out << rettype << " get_" << name;
      out << (is_array ? "(std::size_t index)" : "()");
      out << " {\n";

      if (rettype == "const char*") {
        out << "    return reinterpret_cast<const char*>((char*)base_ptr + "
               "OFFSET_"
            << name << ");\n";
      } else if (is_array) {
        out << "    return *reinterpret_cast<" << rettype
            << "*>((char*)base_ptr + OFFSET_" << parent
            << "_base + index * STRIDE_" << parent << " + OFFSET_" << name
            << ");\n";
      } else {
        out << "    return *reinterpret_cast<" << rettype
            << "*>((char*)base_ptr + OFFSET_" << name << ");\n";
      }

      out << "}\n\n";

    } else if (
        std::regex_match(
            decl, match,
            std::regex(
                R"(void set_(\w+)\((std::size_t index, )?(int|float|double|const char\*) value\);)"))) {
      std::string name = match[1];
      std::string type = match[3];
      bool is_array = decl.find("std::size_t index") != std::string::npos;

      // Detectar nome pai se for array
      std::string parent = name;
      if (is_array && name.find('_') != std::string::npos) {
        parent = name.substr(0, name.find('_'));
      }

      out << "void set_" << name;
      out << (is_array ? "(std::size_t index, " : "(") << type << " value) {\n";

      if (type == "const char*") {
        out << "    std::strncpy((char*)base_ptr + OFFSET_" << name
            << ", value, " << name << "_MAX_LEN);\n";
      } else if (is_array) {
        out << "    *reinterpret_cast<" << type
            << "*>((char*)base_ptr + OFFSET_" << parent
            << "_base + index * STRIDE_" << parent << " + OFFSET_" << name
            << ") = value;\n";
      } else {
        out << "    *reinterpret_cast<" << type
            << "*>((char*)base_ptr + OFFSET_" << name << ") = value;\n";
      }

      out << "}\n\n";

    } else if (std::regex_match(
                   decl, match,
                   std::regex(R"(std::size_t get_(\w+)_count\(\);)"))) {
      std::string name = match[1];
      out << "std::size_t get_" << name << "_count() {\n";
      out << "    return *reinterpret_cast<uint32_t*>((char*)base_ptr + OFFSET_"
          << name << "_count);\n";
      out << "}\n\n";
    } else if (std::regex_match(
                   decl, match,
                   std::regex(
                       R"(void set_(\w+)_count\(std::size_t count\);)"))) {
      std::string name = match[1];
      out << "void set_" << name << "_count(std::size_t count) {\n";
      out << "    *reinterpret_cast<uint32_t*>((char*)base_ptr + OFFSET_"
          << name << "_count) = static_cast<uint32_t>(count);\n";
      out << "}\n\n";
    }
  }
}

void LayoutEngine::load_layout_json(const std::string &path) {
  std::ifstream file(path);
  if (!file) {
    throw std::runtime_error("Erro ao abrir layout.json: " + path);
  }

  json layout = json::parse(file);
  if (!layout.contains("layout")) {
    throw std::runtime_error(
        "layout.json inválido: chave 'layout' não encontrada.");
  }

  build_layout(layout["layout"]);
}
