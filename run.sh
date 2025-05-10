#!/bin/bash

set -e

GREEN="\033[0;32m"
YELLOW="\033[0;33m"
BLUE="\033[0;34m"
RED="\033[0;31m"
RESET="\033[0m"

trap on_exit EXIT

on_exit() {
    EXIT_CODE=$?
    if [ $EXIT_CODE -eq 0 ]; then
        echo -e "\n${YELLOW}[✓] Fim da aplicação com sucesso.${RESET}"
    else
        echo -e "\n${RED}[✗] Aplicação finalizou com erro (código $EXIT_CODE).${RESET}"
    fi
}

if [ ! -d "build" ]; then
    echo -e "\n${YELLOW}[+] Criando diretório de build e configurando CMake...${RESET}"
    cmake -Bbuild -DCMAKE_BUILD_TYPE=Debug
fi

echo -e "\n${BLUE}[+] Compilando o projeto...${RESET}"
cmake --build build

echo -e "\n${GREEN}[+] Executando a aplicação...${RESET}"
echo
./build/main \
  --input layout.json \
  --backing-file /var/run/engine/layout.buf \
  --flatbuffer ./compile/layout.ram \
  --out-dir ./compile/


