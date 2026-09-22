# Open-Scaling

Upscaler de tela em tempo real para **X11 / XFCE**, usando **AMD FSR 1.0 (FidelityFX Super Resolution)** com os filtros **EASU + RCAS**.

O projeto captura a janela de um jogo ou aplicativo em execução e aplica o upscaling da imagem em tempo real, melhorando a nitidez e o desempenho em jogos que rodam em resoluções menores ou sem suporte nativo a FSR.

## Como funciona

- Captura a tela/janela via **X11**.
- Aplica o **FSR 1.0** (EASU para upscaling espacial + RCAS para nitidez).
- Exibe o resultado em uma janela de preview (Vulkan).

## Status

- 🚧 **Beta**: em desenvolvimento ativo.
- 🐛 Ainda possui bugs conhecidos que estou trabalhando para corrigir.
- ⚠️ **Compatibilidade:** roda apenas em **XFCE / X11**. Wayland não é suportado no momento.

## Exemplos

| Nativo (Sem Scaling) | Open-Scaling (FSR 1 - EASU+RCAS) |
|---|---|
| ![Nativo](exemples/hd_nativo_0.9.png) | ![Open-Scaling](exemples/hd_open_scaling.png) |
| ![Nativo 2](exemples/MSAA_hd_nativo.png) | ![Open-Scaling 2](exemples/MSAA_open_upscaling.png) |

## Como rodar

```bash
./Start-make.sh