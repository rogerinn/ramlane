#!/bin/bash

set -e

TARGET_DIR="/var/run/engine"
SIZE="1M"

echo "[+] Criando tmpfs em $TARGET_DIR com tamanho $SIZE..."

# Cria o diretório, se não existir
sudo mkdir -p "$TARGET_DIR"

# Monta tmpfs (não persistente)
sudo mount -t tmpfs -o size=$SIZE tmpfs "$TARGET_DIR"

echo "[✓] tmpfs montado em $TARGET_DIR"
df -h "$TARGET_DIR"
