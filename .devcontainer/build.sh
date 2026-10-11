#!/usr/bin/env bash
# Compile CEmulador en Release, avec les mêmes flags que le CI officiel
# pour Linux (job build-ubuntu de .github/workflows/build.yml).
#
# Relancer ce script après une modification est rapide : CMake/Ninja ne
# recompilent que ce qui a changé. C'est UNIQUEMENT la toute première
# exécution qui est longue (vcpkg compile ses dépendances depuis les
# sources, faute de cache binaire configuré ici — voir la note en bas
# de README_CODESPACE.md si vous voulez accélérer les prochains builds).
set -euo pipefail

cmake -S . -B build \
	-DCMAKE_BUILD_TYPE=release \
	-DCMAKE_C_COMPILER=/usr/bin/clang-15 \
	-DCMAKE_CXX_COMPILER=/usr/bin/clang++-15 \
	-G Ninja \
	-DCMAKE_MAKE_PROGRAM=/usr/bin/ninja

cmake --build build

echo ""
echo "=== Build terminé ==="
echo "Binaire produit : bin/Cemu_release"
echo "Téléchargez-le depuis l'explorateur de fichiers VS Code (clic droit > Download),"
echo "ou zippez-le d'abord si besoin (voir README_CODESPACE.md)."
