// test_layout.cpp
#include "compile/layout_ffi.hpp"
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

int main() {
    constexpr const char* backing = "/tmp/layout_test.buf";

    // 1) Gera o arquivo binário zerado com tamanho exato
    {
        std::ofstream f(backing, std::ios::binary);
        std::vector<char> zero(OFFSET_TOTAL_SIZE, 0);
        f.write(zero.data(), zero.size());
    }

    // 2) Map para memória
    try {
        init_layout_buffer(backing);
    } catch (const std::exception &ex) {
        std::cerr << "Falha init: " << ex.what() << "\n";
        return 1;
    }

    // 3) Teste de campo escalar
    set_balance(55.5);
    assert(std::fabs(get_balance() - 55.5) < 1e-6);

    set_id(1234);
    assert(get_id() == 1234);

    // 4) Teste de string
    const char* hello = "olá";
    set_name(hello);
    assert(std::strcmp(get_name(), hello) == 0);

    // 5) Teste de array orders
    set_orders_count(0);
    // insere manualmente um price e um amount
    set_orders_amount(0, 3.14f);
    set_orders_price(0, 9.87);
    set_orders_side(0, 1);
    set_orders_count(1);

    assert(get_orders_count() == 1);
    assert(std::fabs(get_orders_amount(0) - 3.14f) < 1e-6f);
    assert(std::fabs(get_orders_price(0) - 9.87) < 1e-9);
    assert(get_orders_side(0) == 1);

    // 6) Se tudo passou:
    std::cout << "Todos os testes passaram!\n";
    return 0;
}
