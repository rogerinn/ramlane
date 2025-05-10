#include <layout_engine.hpp>
#include <nlohmann/json.hpp>
#include <iostream>

int main(int argc, char* argv[]) {
    if (argc != 5) {
        std::cerr << "Uso: " << argv[0] << " <layout.json> <memory.buf> <output.ram> <output_dir>\n";
        return 1;
    }

    const std::string json_path = argv[1];
    const std::string ram_path = argv[2];
    const std::string ram_map = argv[3];
    const std::string output_dir = argv[4];
    
    LayoutEngine engine;
    engine.load_layout_json(json_path);
    engine.allocate_memory_from_file(ram_path);

    engine.save_map_flatbuf(ram_map);

    engine.generate_ffi_header(output_dir + "/layout_ffi.hpp");
    engine.generate_ffi_cpp(output_dir + "/layout_ffi.cpp");
    engine.validate_and_format("./compile/layout_ffi.hpp","./compile/layout_ffi.cpp");
    std::cout << "Total buffer size: " << engine.mmap_size()
              << " bytes (" << engine.mmap_size() / 1024.0 << " KB, "
              << engine.mmap_size() / (1024.0 * 1024.0) << " MB)\n";

    return 0;
}
