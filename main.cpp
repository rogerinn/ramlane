#include <layout_engine.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <chrono>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Uso: ./main /caminho/para/layout.buf\n";
        return 1;
    }

    const std::string buf_path = argv[1];

    std::ifstream f("layout.json");
    nlohmann::json layout = nlohmann::json::parse(f);

    LayoutEngine engine(layout);
    engine.allocate_memory_from_file(buf_path);

    std::cout << "Total buffer size: " << engine.mmap_size()
        << " bytes (" << engine.mmap_size() / 1024.0 << " KB, "
        << engine.mmap_size() / (1024.0 * 1024.0) << " MB)\n";

    struct Order {
        double price;
        float amount;
        int32_t side;
    };

    Order o = { 101.25, 5.0f, 1 };

    auto t1 = std::chrono::high_resolution_clock::now();
    engine.insert("orders", &o);
    auto t2 = std::chrono::high_resolution_clock::now();
    auto insert_time = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();

    t1 = std::chrono::high_resolution_clock::now();
    auto* p = reinterpret_cast<Order*>(engine.get("orders", 0));
    t2 = std::chrono::high_resolution_clock::now();
    auto get_time = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();

    if (p) {
        std::cout << "Order[0] → price: " << p->price
                  << ", amount: " << p->amount
                  << ", side: " << p->side << "\n";
    }

    t1 = std::chrono::high_resolution_clock::now();
    engine.pop("orders", 0);
    t2 = std::chrono::high_resolution_clock::now();
    auto pop_time = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();

    int32_t* id = reinterpret_cast<int32_t*>(engine.get("id"));
    if (id) *id = 123;

    std::cout << "ID field value: " << *reinterpret_cast<int32_t*>(engine.get("id")) << "\n";

    std::cout << "\n--- Latências ---\n";
    std::cout << "Insert: " << insert_time << " ns\n";
    std::cout << "Get   : " << get_time << " ns\n";
    std::cout << "Pop   : " << pop_time << " ns\n";
}
