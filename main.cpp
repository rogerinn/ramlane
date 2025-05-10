#include <iostream>
#include <layout_engine.hpp>
#include <nlohmann/json.hpp>
#include <string>

int main(int argc, char *argv[]) {
  std::string json_path;
  std::string backing_file;
  std::string flatbuf_path;
  std::string output_dir;
  bool do_format = false;

  // Parse dos argumentos
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--input" && i + 1 < argc) {
      json_path = argv[++i];
    } else if (arg == "--backing-file" && i + 1 < argc) {
      backing_file = argv[++i];
    } else if (arg == "--flatbuffer" && i + 1 < argc) {
      flatbuf_path = argv[++i];
    } else if (arg == "--out-dir" && i + 1 < argc) {
      output_dir = argv[++i];
    } else if (arg == "--format") {
      do_format = true;
    } else {
      std::cerr << "Argumento desconhecido: " << arg << "\n";
      return 1;
    }
  }

  // Validação mínima
  if (json_path.empty() || backing_file.empty() || flatbuf_path.empty() ||
      output_dir.empty()) {
    std::cerr << "Uso: " << argv[0] << " --input <layout.json>"
              << " --backing-file <memory.buf>"
              << " --flatbuffer <layout.ram>"
              << " --out-dir <output_dir>"
              << " [--format]\n";
    return 1;
  }

  // Pipeline principal
  LayoutEngine engine;
  engine.load_layout_json(json_path);
  engine.allocate_memory_from_file(backing_file);
  engine.save_map_flatbuf(flatbuf_path);
  engine.generate_ffi_header(output_dir + "/layout_ffi.hpp");
  engine.generate_ffi_cpp(output_dir + "/layout_ffi.cpp");

  if (do_format) {
    engine.validate_and_format(output_dir + "/layout_ffi.hpp",
                               output_dir + "/layout_ffi.cpp");
  }

  auto size = engine.mmap_size();
  std::cout << "Total buffer size: " << size << " bytes (" << size / 1024.0
            << " KB, " << size / (1024.0 * 1024.0) << " MB)\n";

  return 0;
}
