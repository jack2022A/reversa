# StageBridge + PlayStage: programa UNICO

Um único motor C++/JUCE: `Source/StageBridgeUnified.cpp`. A compilação desse mesmo arquivo gera Standalone + VST3 para Windows x64 e macOS Intel 10.15; no Mac, também gera AU para usar dentro do MainStage. Não abre navegador nem executa processo C# em paralelo.

## Implementado
- Cinco camadas multissample em WAV/AIFF ou mapas `.psmap` do Ableton Live 11 (faixas de notas e velocidades).
- MIDI do host, sustain, pitch wheel, mute, solo, MIDI ON/OFF e transposição por camada.
- Volume e filtro tonal por camada, master, reverb, medidor e botão PANICO.
- Pad sintetizado contínuo com nota-base, brilho e nível.
- Interface azul/branca, sem teclado virtual, salvamento do estado para a DAW.

## Importação e limitações
- `tools/import_ableton_live11.py` gera mapas `.psmap` do Ableton 11. Áudios continuam no computador do usuário. `tools/inspect_concert.py` inventaria plists de `.concert` sem convertê-los em instrumentos.
- A leitura EXS24/CAF existente no StageBridge C# ainda não foi portada para este motor unificado: não prometer tocar diretamente `.exs`/`.caf`. Não executa presets `.adg`, `.adv` nem instrumentos internos exclusivos da Apple/Ableton.
- O instrumento pode ser hospedado como AU no MainStage, mas não reproduz automaticamente projetos `.concert`.
- Não inclui samples comerciais nem cópias de plugins de terceiros. Ainda requer testes de áudio/MIDI em hardware real.

## Compilação Windows 10/11 64 bits
Requisitos: CMake 3.22+, Git e Visual Studio 2022 (Desktop C++).
```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target StageBridgeUnified_VST3 StageBridgeUnified_Standalone --parallel 2
```
A saída tem `StageBridge PlayStage.exe` e `StageBridge PlayStage.vst3`.

## Compilação macOS Catalina Intel
Requisitos: Xcode, CMake, Git (compilar num Mac; Catalina é o alvo de deployment).
```bash
cmake -S . -B build -G Xcode -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 -DCMAKE_OSX_ARCHITECTURES=x86_64
cmake --build build --config Release --target StageBridgeUnified_Standalone StageBridgeUnified_VST3 StageBridgeUnified_AU --parallel 2
```
A saída inclui `.app`, `.vst3` e `.component`. Instale AU em `~/Library/Audio/Plug-Ins/Components` para testar no MainStage. Assinatura ad-hoc não significa notarização Apple.

## Gerar mapa Ableton
```bash
python3 tools/import_ableton_live11.py "/caminho/projeto.als" --out "/caminho/projeto"
```
Para ZIP coletado, consulte `--help` e use `--extract-samples` apenas localmente. Abra `.psmap` diretamente na camada desejada.

## Verificações
O GitHub Actions na branch `unificado/playstage-stagebridge` compila para ambos os sistemas e envia artefatos de teste, SEM criar Releases ou publicar instaladores antes de revisão.

Originais preservados em `jack2022A/reversa` (branch main) e `jack2022A/StageBridge` (branch rebuild/stagebridge-live).
