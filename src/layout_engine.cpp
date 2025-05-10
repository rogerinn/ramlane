#include "layout_engine.hpp"
#include "layout_map_generated.h" // FlatBuffers schema

#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>

// Para mmap
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

// JSON alias
using json = nlohmann::json;

// -------------------------------
// BUILD LAYOUT
// -------------------------------
void LayoutEngine::load_layout_json(const std::string &path) {
  std::ifstream f(path);
  if (!f)
    throw std::runtime_error("Erro ao abrir layout.json: " + path);
  json root = json::parse(f);
  if (!root.contains("layout"))
    throw std::runtime_error("layout.json inválido: faltando 'layout'");
  build_layout(root["layout"]);
}

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
      field.max_length = def.value("max_length", 256ULL);
      field.size = field.max_length;
    } else if (type == "object" || type == "object[]") {
      bool isArray = (type == "object[]");
      field.type = isArray ? FieldType::Array : FieldType::Object;
      if (isArray) {
        field.max_items = def["max_items"];
        field.count_offset = offset;
        offset += 4;
        field.has_used_flag = true;
      }
      size_t inner_offset = 0;
      for (auto &[key, val] : def["schema"].items()) {
        FieldLayout child;
        child.name = key;
        std::string t = val.get<std::string>();
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
      if (isArray) {
        field.item_stride = inner_offset + (field.has_used_flag ? 1 : 0);
        field.size = field.item_stride * field.max_items;
      } else {
        field.size = inner_offset;
      }
    } else {
      throw std::runtime_error("Tipo desconhecido: " + type);
    }

    field.offset = offset;
    map_.field_index[field.name] = map_.fields.size();
    map_.fields.push_back(field);
    offset += field.size;
  }
  map_.total_size = offset;
}

// -------------------------------
// FLATBUFFERS MAP SAVE/LOAD
// -------------------------------
void LayoutEngine::save_map_flatbuf(const std::string &path) {
  flatbuffers::FlatBufferBuilder builder(1024);
  // Helpers...
  std::function<flatbuffers::Offset<Layout::Field>(const FieldLayout &)>
      build_field;
  build_field = [&](auto const &f) -> auto {
    std::vector<flatbuffers::Offset<Layout::Field>> children;
    for (auto const &c : f.children)
      children.push_back(build_field(c));
    return Layout::CreateField(builder, builder.CreateString(f.name),
                               static_cast<Layout::FieldType>(f.type), f.offset,
                               f.size, f.count_offset, f.item_stride,
                               f.max_items, f.has_used_flag,
                               builder.CreateVector(children));
  };
  std::vector<flatbuffers::Offset<Layout::Field>> vec;
  for (auto const &f : map_.fields)
    vec.push_back(build_field(f));

  auto lm = Layout::CreateLayoutMap(builder, map_.total_size,
                                    builder.CreateVector(vec));
  builder.Finish(lm);

  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<char *>(builder.GetBufferPointer()),
            builder.GetSize());
}

void LayoutEngine::load_map_flatbuf(const std::string &path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    throw std::runtime_error("Não abriu .ram: " + path);
  auto sz = in.tellg();
  in.seekg(0);
  std::vector<char> buf(sz);
  in.read(buf.data(), sz);

  auto lm = Layout::GetLayoutMap(buf.data());
  map_.total_size = lm->total_size();
  map_.fields.clear();
  map_.field_index.clear();

  std::function<FieldLayout(const Layout::Field *)> parse_field;
  parse_field = [&](auto const *f) -> FieldLayout {
    FieldLayout L;
    L.name = f->name()->str();
    L.type = static_cast<FieldType>(f->type());
    L.offset = f->offset();
    L.size = f->size();
    L.count_offset = f->count_offset();
    L.item_stride = f->stride();
    L.max_items = f->max_items();
    L.has_used_flag = f->has_used_flag();
    if (f->children()) {
      for (auto const *c : *f->children()) {
        auto ch = parse_field(c);
        L.field_index[ch.name] = L.children.size();
        L.children.push_back(std::move(ch));
      }
    }
    return L;
  };

  for (auto const *f : *lm->fields()) {
    auto fld = parse_field(f);
    map_.field_index[fld.name] = map_.fields.size();
    map_.fields.push_back(std::move(fld));
  }
}

// -------------------------------
// MMAP / MEMORY
// -------------------------------
void LayoutEngine::allocate_memory_from_file(const std::string &path) {
  size_ = map_.total_size;
  int fd = open(path.c_str(), O_RDWR | O_CREAT, 0600);
  if (fd < 0)
    throw std::runtime_error("open(tmpfs) failed");
  if (ftruncate(fd, size_) < 0) {
    close(fd);
    throw std::runtime_error("ftruncate");
  }
  base_ptr_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (base_ptr_ == MAP_FAILED) {
    close(fd);
    throw std::runtime_error("mmap");
  }
  close(fd);
}

void *LayoutEngine::mmap_base() const { return base_ptr_; }
size_t LayoutEngine::mmap_size() const { return size_; }

// -------------------------------
// INTERNAL INSERT / POP / GET
// -------------------------------
void LayoutEngine::insert(const std::string &field_name, const void *item) {
  auto const &fld = map_.fields[map_.field_index.at(field_name)];
  if (fld.type != FieldType::Array)
    throw std::runtime_error("insert só valids para array");
  uint32_t *cnt =
      reinterpret_cast<uint32_t *>((char *)base_ptr_ + fld.count_offset);
  if (*cnt >= fld.max_items)
    throw std::runtime_error("array cheio");
  size_t base = fld.offset + 4 + (*cnt * fld.item_stride);
  if (fld.has_used_flag) {
    *((char *)base_ptr_ + base) = 1;
    ++base;
  }
  memcpy((char *)base_ptr_ + base, item,
         fld.item_stride - (fld.has_used_flag ? 1 : 0));
  (*cnt)++;
}

void LayoutEngine::pop(const std::string &f, size_t idx) {
  auto const &fld = map_.fields[map_.field_index.at(f)];
  if (fld.type != FieldType::Array)
    throw std::runtime_error("pop só pra array");
  uint32_t *cnt =
      reinterpret_cast<uint32_t *>((char *)base_ptr_ + fld.count_offset);
  if (idx >= *cnt)
    throw std::runtime_error("out of bounds");
  size_t base = fld.offset + 4 + idx * fld.item_stride;
  if (fld.has_used_flag)
    *((char *)base_ptr_ + base) = 0;
}

void *LayoutEngine::get(const std::string &f, size_t idx) {
  auto const &fld = map_.fields[map_.field_index.at(f)];
  if (fld.type == FieldType::Array) {
    uint32_t *cnt =
        reinterpret_cast<uint32_t *>((char *)base_ptr_ + fld.count_offset);
    if (idx >= *cnt)
      return nullptr;
    size_t base = fld.offset + 4 + idx * fld.item_stride;
    if (fld.has_used_flag && *((char *)base_ptr_ + base) == 0)
      return nullptr;
    return (char *)base_ptr_ + base + (fld.has_used_flag ? 1 : 0);
  } else {
    if (idx > 0)
      return nullptr;
    return (char *)base_ptr_ + fld.offset;
  }
}

// -------------------------------
// GENERATE FFI HEADER
// -------------------------------
void LayoutEngine::generate_ffi_header(const std::string &out_path) {
  std::ofstream out(out_path);
  if (!out)
    throw std::runtime_error("Não foi possível abrir " + out_path);

  // 1) Guard e includes básicos
  out << "#pragma once\n"
         "#include <cstddef>\n"
         "#include <cstdint>\n\n";

  // 2) OFFSET_TOTAL_SIZE
  out << "// Tamanho total do buffer (gerado pelo LayoutEngine)\n"
         "constexpr std::size_t OFFSET_TOTAL_SIZE = "
      << map_.total_size << ";\n\n";

  // 3) Geração de OFFSET_<campo> e STRIDE_<array>
  out << "// Offsets e strides gerados\n";
  for (auto const &fld : map_.fields) {
    switch (fld.type) {
    // campos simples
    case FieldType::Int32:
    case FieldType::Int64:
    case FieldType::Float32:
    case FieldType::Float64:
    case FieldType::String:
      out << "constexpr std::size_t OFFSET_" << fld.name << " = " << fld.offset
          << ";\n";
      if (fld.type == FieldType::String) {
        out << "constexpr std::size_t " << fld.name
            << "_MAX_LEN = " << fld.max_length << ";\n";
      }
      break;

    // arrays de objetos
    case FieldType::Array:
      out << "constexpr std::size_t OFFSET_" << fld.name
          << "_count = " << fld.count_offset << ";\n";
      out << "constexpr std::size_t OFFSET_" << fld.name
          << "_base  = " << (fld.offset + 4) << ";\n";
      out << "constexpr std::size_t STRIDE_" << fld.name << "     = "
          << fld.item_stride << ";\n";
      for (auto const &ch : fld.children) {
        out << "constexpr std::size_t OFFSET_" << fld.name << "_" << ch.name
            << " = " << (ch.offset + (fld.has_used_flag ? 1 : 0)) << ";\n";
      }
      break;

    default:
      break;
    }
  }
  out << "\n";

  // 4) Começa bloc o extern "C"
  out << "extern \"C\" {\n\n";

  // 5) init
  out << "void init_layout_buffer(const char* path);\n\n";

  // 6) Struct definitions
  for (auto const &fld : map_.fields) {
    if (fld.type == FieldType::Object) {
      out << "struct " << fld.name << " {\n";
      for (auto const &ch : fld.children) {
        if (ch.type == FieldType::Int32)
          out << "  int    " << ch.name << ";\n";
        else if (ch.type == FieldType::Float32)
          out << "  float  " << ch.name << ";\n";
        else if (ch.type == FieldType::Float64)
          out << "  double " << ch.name << ";\n";
      }
      out << "};\n\n";
    }
    if (fld.type == FieldType::Array) {
      out << "struct " << fld.name << " {\n";
      for (auto const &ch : fld.children) {
        if (ch.type == FieldType::Int32)
          out << "  int    " << ch.name << ";\n";
        else if (ch.type == FieldType::Float32)
          out << "  float  " << ch.name << ";\n";
        else if (ch.type == FieldType::Float64)
          out << "  double " << ch.name << ";\n";
      }
      out << "};\n\n";
    }
  }

  // 7) root_layout
  out << "struct root_layout {\n";
  for (auto const &fld : map_.fields) {
    switch (fld.type) {
    case FieldType::Int32:
    case FieldType::Int64:
      out << "  int    " << fld.name << ";\n";
      break;
    case FieldType::Float32:
      out << "  float  " << fld.name << ";\n";
      break;
    case FieldType::Float64:
      out << "  double " << fld.name << ";\n";
      break;
    case FieldType::String:
      out << "  char   " << fld.name << "[" << fld.max_length << "];\n";
      break;
    case FieldType::Object:
      out << "  struct " << fld.name << " " << fld.name << ";\n";
      break;
    case FieldType::Array:
      out << "  struct " << fld.name << " " << fld.name << "[" << fld.max_items
          << "];\n";
      break;
    default:
      break;
    }
  }
  out << "};\n\n";

  // 8) Assinaturas FFI
  for (auto const &fld : map_.fields) {
    // simples
    if (fld.type == FieldType::Int32) {
      out << "int    get_" << fld.name
          << "();\n"
             "void   set_"
          << fld.name << "(int value);\n\n";
    }
    if (fld.type == FieldType::Float32) {
      out << "float  get_" << fld.name
          << "();\n"
             "void   set_"
          << fld.name << "(float value);\n\n";
    }
    if (fld.type == FieldType::Float64) {
      out << "double get_" << fld.name
          << "();\n"
             "void   set_"
          << fld.name << "(double value);\n\n";
    }
    if (fld.type == FieldType::String) {
      out << "const char* get_" << fld.name
          << "();\n"
             "void         set_"
          << fld.name << "(const char* value);\n\n";
    }

    // sub-objetos
    if (fld.type == FieldType::Object) {
      for (auto const &ch : fld.children) {
        std::string nm = fld.name + "_" + ch.name;
        if (ch.type == FieldType::Int32)
          out << "int    get_" << nm << "(); void set_" << nm << "(int);\n";
        else if (ch.type == FieldType::Float32)
          out << "float  get_" << nm << "(); void set_" << nm << "(float);\n";
        else if (ch.type == FieldType::Float64)
          out << "double get_" << nm << "(); void set_" << nm << "(double);\n";
      }
      out << "\n";
    }

    // arrays
    if (fld.type == FieldType::Array) {
      out << "std::size_t get_" << fld.name
          << "_count();\n"
             "void        set_"
          << fld.name << "_count(std::size_t count);\n\n";
      for (auto const &ch : fld.children) {
        std::string nm = fld.name + "_" + ch.name;
        std::string tp = (ch.type == FieldType::Int32     ? "int"
                          : ch.type == FieldType::Float32 ? "float"
                                                          : "double");
        out << tp << " get_" << nm
            << "(std::size_t index);\n"
               "void set_"
            << nm << "(std::size_t index, " << tp << " value);\n\n";
      }
      out << "void        pop_" << fld.name
          << "(std::size_t index);\n\n"
             "struct "
          << fld.name << " get_" << fld.name
          << "_item(std::size_t index);\n\n"
             "void        get_"
          << fld.name << "_items(std::size_t start, std::size_t count, struct "
          << fld.name << "* out_buffer);\n\n";
    }
  }

  // 9) fecha extern C
  out << "}\n";
  out.close();
}

// -------------------------------
// GENERATE FFI CPP
// -------------------------------
void LayoutEngine::generate_ffi_cpp(const std::string &out_path) {
  // Extrai diretório e nome do arquivo para localizar o header no mesmo
  // diretório
  auto sep = out_path.find_last_of("/\\");
  std::string dir = (sep == std::string::npos ? std::string("")
                                              : out_path.substr(0, sep + 1));
  std::string fname =
      (sep == std::string::npos ? out_path : out_path.substr(sep + 1));
  std::string hdr = fname.substr(0, fname.find_last_of('.')) + ".hpp";
  std::string header_path = dir + hdr;

  // Abre o header gerado no mesmo diretório do out_path
  std::ifstream in(header_path);
  if (!in)
    throw std::runtime_error("Header não encontrado: " + header_path);

  std::ofstream out(out_path);
  // includes top
  out << R"(#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include ")"
      << hdr << R"("

)";

  // Constante de tamanho total
  out << "constexpr std::size_t OFFSET_TOTAL_SIZE = " << map_.total_size
      << ";\n\n";
  out << "void* base_ptr = nullptr;\n\n";

  // Função init
  out << R"(extern "C" void init_layout_buffer(const char* path) {
  int fd = open(path, O_RDWR);
  if (fd < 0) throw std::runtime_error("open failed");
  if (ftruncate(fd, OFFSET_TOTAL_SIZE) < 0) { close(fd); throw std::runtime_error("ftruncate"); }
  base_ptr = mmap(nullptr, OFFSET_TOTAL_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  if (base_ptr == MAP_FAILED) { close(fd); throw std::runtime_error("mmap"); }
  close(fd);
}

)";

  // Declara offsets e strides
  for (auto const &fld : map_.fields) {
    if (fld.type != FieldType::Array && fld.type != FieldType::Object) {
      out << "constexpr std::size_t OFFSET_" << fld.name << " = " << fld.offset
          << ";\n";
      if (fld.type == FieldType::String)
        out << "constexpr std::size_t " << fld.name
            << "_MAX_LEN = " << fld.max_length << ";\n";
    }
    if (fld.type == FieldType::Array) {
      out << "constexpr std::size_t OFFSET_" << fld.name
          << "_count = " << fld.count_offset << ";\n";
      out << "constexpr std::size_t OFFSET_" << fld.name
          << "_base = " << (fld.offset + 4) << ";\n";
      out << "constexpr std::size_t STRIDE_" << fld.name << " = "
          << fld.item_stride << ";\n";
      for (auto const &ch : fld.children)
        out << "constexpr std::size_t OFFSET_" << fld.name << "_" << ch.name
            << " = " << (ch.offset + (fld.has_used_flag ? 1 : 0)) << ";\n";
    }
  }
  out << "\n";

  // Parse declarações do header
  std::vector<std::string> decls;
  std::string line;
  while (std::getline(in, line)) {
    if (line.find("get_") != std::string::npos ||
        line.find("set_") != std::string::npos ||
        line.find("pop_") != std::string::npos)
      decls.push_back(line);
  }

  // Implementa cada getter/setter/pop/get_item
  for (auto const &d : decls) {
    std::smatch m;
    // int get_X();
    if (std::regex_match(d, m, std::regex(R"(int get_(\w+)\(\);)"))) {
      auto nm = m[1];
      out << "int get_" << nm
          << "() { return *reinterpret_cast<int*>((char*)base_ptr + OFFSET_"
          << nm << "); }\n\n";
    }
    // void set_X(int value);
    else if (std::regex_match(d, m,
                              std::regex(R"(void set_(\w+)\(int value\);)"))) {
      auto nm = m[1];
      out << "void set_" << nm
          << "(int v) { *reinterpret_cast<int*>((char*)base_ptr + OFFSET_" << nm
          << ") = v; }\n\n";
    }
    // float get_X();
    else if (std::regex_match(d, m, std::regex(R"(float get_(\w+)\(\);)"))) {
      auto nm = m[1];
      out << "float get_" << nm
          << "() { return *reinterpret_cast<float*>((char*)base_ptr + OFFSET_"
          << nm << "); }\n\n";
    }
    // void set_X(float value);
    else if (std::regex_match(
                 d, m, std::regex(R"(void set_(\w+)\(float value\);)"))) {
      auto nm = m[1];
      out << "void set_" << nm
          << "(float v) { *reinterpret_cast<float*>((char*)base_ptr + OFFSET_"
          << nm << ") = v; }\n\n";
    }
    // double get_X();
    else if (std::regex_match(d, m, std::regex(R"(double get_(\w+)\(\);)"))) {
      auto nm = m[1];
      out << "double get_" << nm
          << "() { return *reinterpret_cast<double*>((char*)base_ptr + OFFSET_"
          << nm << "); }\n\n";
    }
    // void set_X(double value);
    else if (std::regex_match(
                 d, m, std::regex(R"(void set_(\w+)\(double value\);)"))) {
      auto nm = m[1];
      out << "void set_" << nm
          << "(double v) { *reinterpret_cast<double*>((char*)base_ptr + OFFSET_"
          << nm << ") = v; }\n\n";
    }
    // const char* get_X();
    else if (std::regex_match(d, m,
                              std::regex(R"(const char\* get_(\w+)\(\);)"))) {
      auto nm = m[1];
      out << "const char* get_" << nm
          << "() { return reinterpret_cast<const char*>((char*)base_ptr + "
             "OFFSET_"
          << nm << "); }\n\n";
    }
    // void set_X(const char* value);
    else if (std::regex_match(
                 d, m,
                 std::regex(R"(void set_(\w+)\(const char\* value\);)"))) {
      auto nm = m[1];
      out << "void set_" << nm
          << "(const char* v) { strncpy((char*)base_ptr + OFFSET_" << nm
          << ", v, " << nm << "_MAX_LEN); }\n\n";
    }
    // std::size_t get_arr_count();
    else if (std::regex_match(
                 d, m, std::regex(R"(std::size_t get_(\w+)_count\(\);)"))) {
      auto nm = m[1];
      out << "std::size_t get_" << nm
          << "_count() { return *reinterpret_cast<uint32_t*>((char*)base_ptr + "
             "OFFSET_"
          << nm << "_count); }\n\n";
    }
    // void set_arr_count(std::size_t count);
    else if (std::regex_match(
                 d, m,
                 std::regex(R"(void set_(\w+)_count\(std::size_t count\);)"))) {
      auto nm = m[1];
      out << "void set_" << nm
          << "_count(std::size_t c) { "
             "*reinterpret_cast<uint32_t*>((char*)base_ptr + OFFSET_"
          << nm << "_count) = static_cast<uint32_t>(c); }\n\n";
    }
    // float get_arr_field(std::size_t index);
    else if (std::regex_match(
                 d, m,
                 std::regex(
                     R"(float get_(\w+)_(\w+)\(std::size_t index\);)"))) {
      auto arr = m[1], fld_ch = m[2];
      out << "float get_" << arr << "_" << fld_ch
          << "(std::size_t i) { return "
             "*reinterpret_cast<float*>((char*)base_ptr + OFFSET_"
          << arr << "_base + i * STRIDE_" << arr << " + OFFSET_" << arr << "_"
          << fld_ch << "); }\n\n";
    }
    // void set_arr_field(std::size_t index, float value);
    else if (
        std::regex_match(
            d, m,
            std::regex(
                R"(void set_(\w+)_(\w+)\(std::size_t index, float value\);)"))) {
      auto arr = m[1], fld_ch = m[2];
      out << "void set_" << arr << "_" << fld_ch
          << "(std::size_t i, float v) { "
             "*reinterpret_cast<float*>((char*)base_ptr + OFFSET_"
          << arr << "_base + i * STRIDE_" << arr << " + OFFSET_" << arr << "_"
          << fld_ch << ") = v; }\n\n";
    }
    // double get_arr_field(std::size_t index);
    else if (std::regex_match(
                 d, m,
                 std::regex(
                     R"(double get_(\w+)_(\w+)\(std::size_t index\);)"))) {
      auto arr = m[1], fld_ch = m[2];
      out << "double get_" << arr << "_" << fld_ch
          << "(std::size_t i) { return "
             "*reinterpret_cast<double*>((char*)base_ptr + OFFSET_"
          << arr << "_base + i * STRIDE_" << arr << " + OFFSET_" << arr << "_"
          << fld_ch << "); }\n\n";
    }
    // void set_arr_field(std::size_t index, double value);
    else if (
        std::regex_match(
            d, m,
            std::regex(
                R"(void set_(\w+)_(\w+)\(std::size_t index, double value\);)"))) {
      auto arr = m[1], fld_ch = m[2];
      out << "void set_" << arr << "_" << fld_ch
          << "(std::size_t i, double v) { "
             "*reinterpret_cast<double*>((char*)base_ptr + OFFSET_"
          << arr << "_base + i * STRIDE_" << arr << " + OFFSET_" << arr << "_"
          << fld_ch << ") = v; }\n\n";
    }
    // int get_arr_field(std::size_t index);
    else if (std::regex_match(
                 d, m,
                 std::regex(R"(int get_(\w+)_(\w+)\(std::size_t index\);)"))) {
      auto arr = m[1], fld_ch = m[2];
      out << "int get_" << arr << "_" << fld_ch
          << "(std::size_t i) { return *reinterpret_cast<int*>((char*)base_ptr "
             "+ OFFSET_"
          << arr << "_base + i * STRIDE_" << arr << " + OFFSET_" << arr << "_"
          << fld_ch << "); }\n\n";
    }
    // void set_arr_field(std::size_t index, int value);
    else if (
        std::regex_match(
            d, m,
            std::regex(
                R"(void set_(\w+)_(\w+)\(std::size_t index, int value\);)"))) {
      auto arr = m[1], fld_ch = m[2];
      out << "void set_" << arr << "_" << fld_ch
          << "(std::size_t i, int v) { *reinterpret_cast<int*>((char*)base_ptr "
             "+ OFFSET_"
          << arr << "_base + i * STRIDE_" << arr << " + OFFSET_" << arr << "_"
          << fld_ch << ") = v; }\n\n";
    }
    // void pop_arr(std::size_t index);
    else if (std::regex_match(
                 d, m, std::regex(R"(void pop_(\w+)\(std::size_t index\);)"))) {
      auto arr = m[1];
      out << "void pop_" << arr
          << "(std::size_t i) { *((char*)base_ptr + OFFSET_" << arr
          << "_base + i * STRIDE_" << arr << ") = 0; }\n\n";
    }
    // struct get_arr_item(std::size_t index);
    else if (std::regex_match(
                 d, m,
                 std::regex(
                     R"(struct (\w+) get_(\w+)_item\(std::size_t index\);)"))) {
      auto st = m[1], arr = m[2];
      out << "struct " << st << " get_" << arr << "_item(std::size_t i) {\n";
      out << "  struct " << st << " o;\n";
      out << "  memcpy(&o, (char*)base_ptr + OFFSET_" << arr
          << "_base + i * STRIDE_" << arr << " + "
          << (map_.fields[map_.field_index[arr]].has_used_flag ? 1 : 0)
          << ", sizeof(o));\n";
      out << "  return o;\n";
      out << "}\n\n";
    }
  }

  out.close();
}

void LayoutEngine::validate_and_format(const std::string &header_path,
                                       const std::string &cpp_path) {
  // 1) verifica se os arquivos existem
  std::ifstream f1(header_path);
  if (!f1)
    throw std::runtime_error("Arquivo não encontrado: " + header_path);
  std::ifstream f2(cpp_path);
  if (!f2)
    throw std::runtime_error("Arquivo não encontrado: " + cpp_path);
  f1.close();
  f2.close();

  // 2) formata ambos com clang-format, estilo definido em .clang-format
  int ret = std::system(("clang-format -style=file -i " + header_path).c_str());
  if (ret != 0)
    throw std::runtime_error("clang-format falhou no header: " + header_path);
  else
    std::cout << "Header formatado com sucesso: " << header_path << std::endl;

  ret = std::system(("clang-format -style=file -i " + cpp_path).c_str());
  if (ret != 0)
    throw std::runtime_error("clang-format falhou no cpp: " + cpp_path);
  else
    std::cout << "CPP formatado com sucesso: " << cpp_path << std::endl;
}
