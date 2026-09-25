# Open-Scaling

Upscaler de tela em tempo real para **X11 / XFCE**, usando **AMD FSR 1.0 (FidelityFX Super Resolution)** com os filtros **EASU + RCAS**.

Captura a janela de um jogo ou aplicativo em execução e aplica o upscaling da imagem em tempo real, melhorando a nitidez e o desempenho em jogos que rodam em resoluções menores ou sem suporte nativo a FSR **(ainda com bugs conhecidos de input)**.

## 🛠️ Como funciona

- Captura a tela/janela via **X11**.
- Aplica o **FSR 1.0** (EASU para upscaling espacial + RCAS para nitidez).
- Exibe o resultado em uma janela de preview via **Vulkan**.

## 📊 Status do Projeto

- ⚠️ **Beta**: Em desenvolvimento ativo.
- 🪳 Possui bugs conhecidos de input/captura que estão sendo corrigidos.
- ⚠️ **Compatibilidade:** Roda apenas em **XFCE / X11**. O protocolo Wayland não é suportado no momento.
- ❗ **Desempenho:** Pode apresentar gargalos em CPUs e GPUs mais antigas (testes realizados na plataforma LGA 1155 com memória DDR3).

## 📝 Notas de Atualização (Updates)

- **Fixes no método de entrada:** Pequenas correções aplicadas, mas o input ainda pode falhar dependendo do programa. Recomenda-se o uso em jogos que possuem ponteiro/indicador próprio, pois o Open-Scaling ainda não traz o ponteiro do desktop automaticamente para dentro da janela de preview.
- **Tempo de resposta:** Melhorias relevantes na latência de exibição. O objetivo atual é reduzir ainda mais o *image-lag* (tempo de resposta em ms).
- **Otimização:** Menor overhead na GPU e melhoria significativa na nitidez e legibilidade de textos capturados.
- **Script Interativo:** Adicionado o `start.sh`, automatizando todo o processo desde a compilação do Open-Scaling até a execução do FSR 1.

### Opções do Menu Interativo (`start.sh`):
```text
 [1] Rodar FSR (upscale com janela de preview)
 [2] Preview (sem upscale, apenas visualização da captura)
 [3] Benchmark
 [4] Snapshot (capturar 1 frame de uma janela)
 [5] Listar janelas disponíveis
 ------------------------------------------------------
 [6] Compilar / Recompilar
 [7] Limpar build
 [8] Ver README
 [9] Ajuda do binário (--help)
```

## 🖼️ Exemplos

*Nota: Os exemplos visuais abaixo podem não refletir com total fidelidade o estado mais recente de desenvolvimento do projeto.*

| Nativo (Sem Scaling) | Open-Scaling (FSR 1 - EASU + RCAS) |
|---|---|
| ![Nativo](exemples/hd_nativo_0.9.png) | ![Open-Scaling](exemples/hd_open_scaling.png) |
| ![MSAA Nativo](exemples/MSAA_hd_nativo.png) | ![MSAA Open Upscaling](exemples/MSAA_open_upscaling.png) |
