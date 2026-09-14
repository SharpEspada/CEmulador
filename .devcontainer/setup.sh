#!/usr/bin/env bash
# Exécuté automatiquement par devcontainer.json (postCreateCommand).
# Reprend exactement la liste de paquets du job "build-ubuntu" de
# .github/workflows/build.yml, pour rester fidèle au build officiel.
set -euo pipefail

echo "=== [1/3] Paquets système (identiques au CI officiel + autotools) ==="
sudo apt update -qq
sudo apt install -y \
	clang-15 cmake freeglut3-dev libgcrypt20-dev libglm-dev \
	libgtk-3-dev libpulse-dev libsecret-1-dev libsystemd-dev \
	libudev-dev nasm ninja-build libbluetooth-dev git \
	autoconf automake libtool m4 pkg-config
	# ^ ces 5 derniers ne sont PAS dans la liste du CI officiel : les
	# runners GitHub Actions "ubuntu-22.04" les ont préinstallés par
	# défaut sur leur image, mais l'image de base du devcontainer est
	# plus minimale. Sans eux, vcpkg échoue sur libusb (qui a besoin
	# d'autoreconf) — découvert lors d'un premier build réel.

echo "=== [2/3] Sous-module vcpkg ==="
# Codespaces ne clone pas toujours les sous-modules par défaut selon la
# config du dépôt ; ceci est sans effet (rapide) s'il est déjà présent.
git submodule update --init --recursive

echo "=== [3/3] Bootstrap de vcpkg ==="
bash ./dependencies/vcpkg/bootstrap-vcpkg.sh

echo ""
echo "=== Terminé. Pour compiler Cemu, lancez : ==="
echo "    bash .devcontainer/build.sh"
echo "(c'est une étape séparée et longue — voir README_CODESPACE.md)"
