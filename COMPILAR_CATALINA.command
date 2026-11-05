#!/bin/bash
# Build *real* Intel Mach-O native .app on a Mac, not a Safari launcher.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"

if [ "$(uname -s)" != "Darwin" ]; then
    echo "ERRO: este projeto tem de ser compilado no macOS, com Xcode." >&2
    exit 1
fi

for tool in cmake xcodebuild git ditto; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERRO: $tool nao encontrado. Instale Xcode e CMake." >&2
        exit 1
    fi
done

mkdir -p "$REPO_DIR/Saida"

cmake -S "$REPO_DIR" -B "$REPO_DIR/.build" -G Xcode \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 \
    -DCMAKE_OSX_ARCHITECTURES=x86_64

cmake --build "$REPO_DIR/.build" --config Release --parallel 2

APP_PATH="$(find "$REPO_DIR/.build" -type d -name 'PlayStage VST3 Catalina.app' \
    -path '*/Release/*' -print -quit)"

if [ -z "$APP_PATH" ]; then
    echo "ERRO: o build nao encontrou PlayStage VST3 Catalina.app" >&2
    exit 1
fi

# Do not sign with another user's identity or modify security settings.
codesign --force --deep --sign - "$APP_PATH"
ditto -c -k --sequesterRsrc --keepParent "$APP_PATH" \
    "$REPO_DIR/Saida/PlayStage_VST3_Catalina_Intel.zip"

echo "App nativo compactado em:"
echo "  $REPO_DIR/Saida/PlayStage_VST3_Catalina_Intel.zip"
echo "O arquivo contem codigo Mach-O real, nao um link de navegador."
