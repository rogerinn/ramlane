# ramlane

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

## Índice

* [Descrição](#descrição)
* [Funcionalidades](#funcionalidades)
* [Pré-requisitos](#pré-requisitos)
* [Instalação](#instalação)
* [FlatBuffers Schema](#flatbuffers-schema)
* [Estrutura do Projeto](#estrutura-do-projeto)
* [Uso CLI](#uso-cli)
* [Uso como Biblioteca](#uso-como-biblioteca)
* [Formato do JSON de Layout](#formato-do-json-de-layout)
* [Geração de Código](#geração-de-código)
* [Testes](#testes)
* [Docker](#docker)
* [Contribuição](#contribuição)
* [Licença](#licença)

## Descrição

`ramlane` é uma biblioteca e ferramenta de linha de comando para gerar mapeamentos de memória em C++ a partir de definições JSON. Ela oferece duas modalidades de uso:

1. **CLI**: aplicativo que parseia argumentos, carrega o layout, aloca/mapeia memória, serializa em FlatBuffers e gera código FFI.
2. **API em C++**: classe `LayoutEngine` para integrar diretamente em seu código, controlando quando salvar ou gerar cada artefato.

Com `ramlane` você pode:

* Carregar apenas o layout e gerar o arquivo FlatBuffers (`.ram`) sem escrever código.
* Usar um `.ram` existente para gerar código FFI sem precisar do `layout.json` original.
* Inicializar um buffer de memória (`mmap`) e manter vivo para múltiplas operações.
* Gerar código FFI C++ (`layout_ffi.hpp`/`.cpp`) a partir do JSON ou de um `.ram` carregado, sem reserializar.

## Funcionalidades

### 1. Geração de Mapa de Layout

- **Objetivo**: A partir do `layout.json`, gera o arquivo FlatBuffers (`.ram`) contendo o `LayoutMap` serializado.
- **O que inclui**:
  - Validação de tipos, nomes duplicados e limites (max_length para strings, max_items para arrays).
  - Cálculo recursivo de offsets, tamanhos e strides para campos simples, objetos e arrays.
  - Serialização otimizada usando FlatBuffers.
- **Chamadas de API**:
  ```cpp
  LayoutEngine engine;
  engine.load_layout_json("layout.json");      // parse e validação
  engine.save_map_flatbuf("layout.ram");       // grava o .ram
  ```
- **Quando usar**:  
  - Em build-time ou pipelines de CI para gerar o artefato de layout uma única vez.

### 2. Geração de Códigos FFI (C++)

- **Objetivo**: A partir do `.ram`, cria automaticamente headers e fontes C++ com getters/setters, pop e iteração.
- **O que inclui**:
  - `layout_ffi.hpp` com constantes de offset, definição de structs e assinaturas C.
  - `layout_ffi.cpp` com implementação de funções de acesso (get/set/pop/get_item).
  - Formatação automática opcional via `clang-format`.
- **Chamadas de API**:
  ```cpp
  LayoutEngine engine;
  engine.load_map_flatbuf("layout.ram");                            // sem JSON
  engine.generate_ffi_header("generated/layout_ffi.hpp");
  engine.generate_ffi_cpp("generated/layout_ffi.cpp");
  ```
- **Quando usar**:  
  - Para gerar bindings sempre que o `.ram` for alterado, sem precisar recarregar o JSON.

### 3. Uso Direto em C++ (Runtime)

- **Objetivo**: Carregar e manipular o buffer em memória (file-backed via mmap), sem necessidade de geração de código adicional.
- **O que inclui**:
  - `allocate_memory_from_file(path)` para abrir/criar arquivo de backing e mapear via `mmap`.
  - APIs dinâmicas de inserção (`insert`), remoção (`pop`) e leitura (`get`, `get_<campo>`).
  - Gerenciamento de contadores para arrays e flags de uso interno.
- **Chamadas de API**:
  ```cpp
  LayoutEngine engine;
  engine.allocate_memory_from_file("memory.buf");                  // mmap
  engine.load_map_flatbuf("layout.ram");                           // carrega offsets

  // Operações em campos simples
  engine.set_id(123);
  int id = engine.get_id();

  // Operações em arrays de objetos
  MyOrder ord{/*...*/};
  engine.insert("orders", &ord);
  size_t cnt = engine.get_orders_count();
  auto ptr = engine.get("orders", 0);
  engine.pop("orders", 0);
  ```
- **Quando usar**:  
  - Em aplicações de runtime para IPC de baixa latência ou compartilhamento entre processos.

## Pré-requisitos

* **C++17** (g++ 9+ ou clang 10+)
* **CMake** (>= 3.15)
* **FlatBuffers** (`flatc` CLI)
* **nlohmann/json** (header-only)
* **clang-format** (opcional)
* **bash** (para scripts)

## Instalação

```bash
git clone https://github.com/rogerinn/ramlane.git
cd ramlane
sudo apt-get update && sudo apt-get install -y flatbuffers-compiler
```

## FlatBuffers Schema

Definição em `flatbuffers/layout_map.fbs`:

```fbs
namespace Layout;

enum FieldType : byte {
  Int32,
  Int64,
  Float32,
  Float64,
  String,
  Object,
  Array
}

table Field {
  name: string;
  type: FieldType;
  offset: uint32;
  size: uint32;
  count_offset: uint32;
  stride: uint32;
  max_items: uint32;
  has_used_flag: bool;
  children: [Field];
}

table LayoutMap {
  total_size: uint32;
  fields: [Field];
}

root_type LayoutMap;
```

Para gerar o header (não versionado no repositório):

```bash
flatc --cpp -o include/ flatbuffers/layout_map.fbs
```

Produz `include/layout_map_generated.h`.

## Estrutura do Projeto

```
ramlane/
├── CMakeLists.txt            # Configuração do build
├── ci.yml                    # Pipeline CI (GitHub Actions)
├── Dockerfile                # Docker multi-stage
├── layout.json               # Exemplo de definição de layout
├── flatbuffers/
│   └── layout_map.fbs        # Schema FlatBuffers
├── include/                  # Headers públicos
│   ├── layout_engine.hpp     # API da engine
│   └── layout_map_generated.h# Gerado pelo flatc
├── src/                      # Implementação interna
│   ├── layout_engine.cpp     # Carrega JSON e gerencia mmap/FlatBuffers
│   └── codegen.cpp           # Geração de código FFI C++
├── main.cpp                  # CLI principal (parsing de flags)
├── run.sh                    # Script helper (build e execução)
├── tests/                    # Testes de integração
│   └── layout_test.cpp
├── scripts/
│   └── formatter.sh          # Aplica clang-format
├── .gitignore
└── LICENSE
```

## Uso CLI

### Compilação Manual

Para usar o CLI via binário diretamente, primeiro compile o projeto:

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
cmake --build .
```

Em seguida, execute o binário:

```bash
./build/main \
  --input layout.json \
  --backing-file /var/run/engine/layout.buf \
  --flatbuffer ./compile/layout.ram \
  --out-dir ./compile/ [--format]
```

### Uso com `run.sh`

O repositório inclui um script `run.sh` para simplificar build e execução:

```bash
#!/bin/bash
set -e

GREEN="[0;32m"
YELLOW="[0;33m"
BLUE="[0;34m"
RED="[0;31m"
RESET="[0m"

trap on_exit EXIT

on_exit() {
    EXIT_CODE=$?
    if [ $EXIT_CODE -eq 0 ]; then
        echo -e "
${YELLOW}[✓] Fim da aplicação com sucesso.${RESET}"
    else
        echo -e "
${RED}[✗] Aplicação finalizou com erro (código $EXIT_CODE).${RESET}"
    fi
}

if [ ! -d "build" ]; then
    echo -e "
${YELLOW}[+] Criando diretório de build e configurando CMake...${RESET}"
    cmake -Bbuild -DCMAKE_BUILD_TYPE=Debug
fi

echo -e "
${BLUE}[+] Compilando o projeto...${RESET}"
cmake --build build

echo -e "
${GREEN}[+] Executando a aplicação...${RESET}"
echo
./build/main \
  --input layout.json \
  --backing-file /var/run/engine/layout.buf \
  --flatbuffer ./compile/layout.ram \
  --out-dir ./compile/
```

Para executar com um comando único:

```bash
bash run.sh
```

Esse script:

1. Cria e configura o diretório `build` (se não existir).
2. Compila o projeto em modo Debug.
3. Executa o binário com parâmetros padrão.
4. Exibe mensagens coloridas de sucesso ou erro ao final.

### Flags (alternativa)

```bash
./ramlane \
  --input layout.json \
  --backing-file memory.buf \
  --flatbuffer layout.ram \
  --out-dir generated \
  [--format]
```

### Positional (alternativa)

```bash
./ramlane layout.json memory.buf layout.ram generated [--format]
```

Em qualquer modo, o CLI realiza:

1. `allocate_memory_from_file(path)` — abre/cria arquivo de backing e mapeia via `mmap`.
2. `load_layout_json(layout.json)` — parse do JSON.
3. `save_map_flatbuf(layout.ram)` — grava o `.ram`.
4. `generate_ffi(output_dir, format)` — gera os arquivos FFI e opcionalmente formata.

## Uso como Biblioteca

Você pode usar a API C++ diretamente sem CLI, por exemplo:

```cpp
#include "layout_engine.hpp"

int main() {
    LayoutEngine engine;
    // 1) Abre/cria e mapeia memória via arquivo
    engine.allocate_memory_from_file("memory.buf");
    // 2) Carrega JSON de layout
    engine.load_layout_json("layout.json");
    // 3) Serializa em FlatBuffers (opcional)
    engine.save_map_flatbuf("layout.ram");
    // 4) Gera código FFI C++:
    engine.generate_ffi_header("generated/layout_ffi.hpp");
    engine.generate_ffi_cpp("generated/layout_ffi.cpp");
    return 0;
}
```

### Principais métodos da API:

* `allocate_memory_from_file(const std::string& path)` — abre/cria o arquivo de backing e mapeia em memória via `mmap`.
* `load_layout_json(const std::string& path)` — parse do JSON e cálculo de offsets.
* `save_map_flatbuf(const std::string& path)` — grava o layout em FlatBuffers (`.ram`).
* `generate_ffi_header(const std::string& output_path)` — gera o arquivo header `layout_ffi.hpp`.
* `generate_ffi_cpp(const std::string& output_path)` — gera o arquivo fonte `layout_ffi.cpp`.
* `load_map_flatbuf(const std::string& path)` — carrega layout previamente serializado (`.ram`).
* `get_layout()` — retorna o objeto `LayoutMap` (estrutura interna) usado para geração.

## Formato do JSON de Layout

O arquivo `layout.json` deve conter apenas a chave `layout`, que lista os campos a serem mapeados:

```json
{
  "layout": {
    /* definição dos campos */
  }
}
```

### 1. Definição de Campos (`layout`)

Cada entrada em `layout` mapeia um campo, definido por um objeto com as seguintes propriedades:

| Propriedade  | Tipo   | Obrigatório em             | Descrição                                                                                                                  |
| ------------ | ------ | -------------------------- | -------------------------------------------------------------------------------------------------------------------------- |
| `type`       | string | sempre                     | Tipo de dado. Valores válidos: `int32`, `uint32`, `int64`, `uint64`, `float32`, `float64`, `string`, `object`, `object[]`. |
| `max_length` | uint32 | quando `type="string"`     | Comprimento máximo em bytes para campos `string`.                                                                          |
| `schema`     | objeto | quando `object`/`object[]` | Define subcampos e seus tipos. Ex.: `{ "campo": "tipo", ... }`.                                                            |
| `max_items`  | uint32 | quando `type="object[]"`   | Número máximo de elementos em arrays de objetos. Deve ser ≥ 1.                                                             |

### 2. Regras e Limites

* **Campos**: máximo de `1024` entradas em `layout`.
* **Strings**: todo `type="string"` requer `max_length`.
* **Objetos** (`object`): `schema` deve possuir ao menos um subcampo.
* **Arrays de Objetos** (`object[]`): requer `schema` e `max_items`.
* **Aninhamento**: objetos podem conter subcampos do tipo `object`, com profundidade recomendada de até `5` níveis.

### 3. Exemplo de `layout.json`

```json
{
  "layout": {
    "id": {
      "type": "int32"
    },
    "name": {
      "type": "string",
      "max_length": 64
    },
    "balance": {
      "type": "float64"
    },
    "config": {
      "type": "object",
      "schema": {
        "active": "int32",
        "threshold": "float32"
      }
    },
    "orders": {
      "type": "object[]",
      "max_items": 100,
      "schema": {
        "price": "float64",
        "amount": "float32",
        "side": "int32"
      }
    }
  }
}
```

Com essas definições, o `LayoutEngine` calcula offsets, tamanhos e strides de cada campo, validando limites e gerando o mapeamento de memória corretamente.

## Geração de Código

1. **Parse** JSON → offsets, stride, validações.
2. **Validação**: limites de `max_length`, `max_items` e total de campos.
3. **Mmap** de arquivo zerado.
4. **FlatBuffers**: grava `.ram`.
5. **CodeGen**:

   * `layout_ffi.hpp` com funções:

     ```cpp
     <tipo> get_<campo>(size_t idx=0);
     void set_<campo>(size_t idx, <tipo>);
     size_t get_<orders>_count();
     auto get_<orders>_item(size_t idx);
     ```
   * `layout_ffi.cpp` com ponteiros base + offset.
6. (Opcional) `clang-format`.

## Testes

```bash
cd tests && mkdir build && cd build
cmake .. && make
./layout_test
```

Valida leitura/escrita, limites e contagem.

## Docker

```bash
docker build -t ramlane:latest .

docker run --rm -v $(pwd):/data ramlane:latest \
  layout.json /data/ram.bin /data/layout.ram /data/generated [--format]
```

A imagem final contém apenas o binário `ramlane`.

## Contribuição

1. Fork no GitHub e crie branch (`feature/x`).
2. Adicione testes e documentação.
3. Abra PR contra `main`.

## Licença

Este projeto está licenciado sob a [MIT License](LICENSE).
