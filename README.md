# Open-Scaling v0.30 (Beta)

![Status](https://img.shields.io/badge/status-beta-yellow)
![License](https://img.shields.io/badge/license-MIT-blue)
![Platform](https://img.shields.io/badge/platform-Linux%20%2F%20X11-lightgrey)

Upscaler de tela em tempo real para **X11 / XFCE**, usando **AMD FSR 1.0** (FidelityFX Super Resolution) com os filtros **EASU + RCAS**.

Captura a janela de um jogo ou aplicativo em execução e aplica o upscaling da imagem em tempo real, melhorando a nitidez e o desempenho em jogos que rodam em resoluções menores ou sem suporte nativo a FSR **(ainda com bugs conhecidos de input)**.

---

## 🛠️ Como funciona

- Captura a tela/janela via **X11**.
- Aplica o **FSR 1.0** (EASU para upscaling espacial + RCAS para nitidez).
- Exibe o resultado em uma janela de preview via **Vulkan**.

---

## 📊 Status do Projeto

- ⚠️ **Beta**: Em desenvolvimento ativo.
- 🪳 Possui bugs conhecidos de input/captura que estão sendo corrigidos.
- ⚠️ **Compatibilidade:** Roda apenas em **XFCE / X11**. O protocolo Wayland não é suportado no momento.
- ❗ **Desempenho:** Pode apresentar gargalos em CPUs e GPUs mais antigas (testes realizados na plataforma LGA 1155 com memória DDR3).

---

## 📦 Requisitos

- **SO:** Linux com servidor gráfico **X11** (XFCE recomendado)
- **GPU:** Compatível com **Vulkan** (drivers AMD, Intel ou NVIDIA atualizados)
- **Compilador:** `gcc` ou `clang` com suporte a C++17
- **Bibliotecas de desenvolvimento:**
  - `libvulkan-dev`
  - `libx11-dev`
  - `libxcomposite-dev`
  - `libxrandr-dev`
  - `libxinerama-dev`
  - `libxcursor-dev`
  - `libxi-dev`
  - `cmake` (>= 3.15) e `make`
- **Runtime:** `vulkan-tools` (opcional, para diagnóstico)

> Em distros baseadas em Debian/Ubuntu:
> ```bash
> sudo apt install build-essential cmake libvulkan-dev libx11-dev \
>   libxcomposite-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
> ```

---

## ⚙️ Instalação / Build

1. Clone o repositório:
   ```bash
   git clone https://github.com/vinihoelzel-dev/Open-Scaling
   cd Open-Scaling
   ```

2. Torne os scripts executáveis:
   ```bash
   chmod +x start.sh ui.sh
   ```

3. Compile e execute pelo menu interativo:
   ```bash
   ./start.sh
   ```

   Ou abra direto o launcher gráfico:
   ```bash
   ./ui.sh
   ```

---

## 📝 Notas de Atualização (Updates)

- **Fixes no método de entrada:** Pequenas correções aplicadas, mas o input ainda pode falhar dependendo do programa. Recomenda-se o uso em jogos que possuem ponteiro/indicador próprio, pois o Open-Scaling ainda não traz o ponteiro do desktop automaticamente para dentro da janela de preview.
- **Tempo de resposta:** Melhorias relevantes na latência de exibição. O objetivo atual é reduzir ainda mais o *image-lag* (tempo de resposta em ms).
- **Otimização:** Menor overhead na GPU e melhoria significativa na nitidez e legibilidade de textos capturados.
- **Script Interativo:** Adicionado o `start.sh`, automatizando todo o processo desde a compilação do Open-Scaling até a execução do FSR 1.0.
- **Interface gráfica:** Novo launcher em ImGui + AppImage distribuível.

---

## 🚀 Scripts

- **`start.sh`** — Menu interativo no terminal: compilar, rodar FSR, preview, benchmark, snapshot e listar janelas.
- **`ui.sh`** — Atalho para abrir direto a interface gráfica (launcher ImGui), sem passar pelo menu do terminal.

```bash
# abre o launcher gráfico
./ui.sh
```

### Opções do Menu Interativo (`start.sh`)

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

---

## ⌨️ Interface

Interface gráfica baseada em **ImGui**, com painel de status, lista de janelas alvo, seleção de qualidade, ações (FSR, preview, benchmark, snapshot), manutenção (compilar / limpar build) e console de saída em tempo real.

![Interface do Launcher](exemplos/Launcher_interface.png)

---

## 🖼️ Exemplos

> *Nota: Os exemplos visuais abaixo podem não refletir com total fidelidade o estado mais recente de desenvolvimento do projeto.*

| Nativo (Sem Scaling) | Open-Scaling (FSR 1.0 — EASU + RCAS) |
|---|---|
| ![Nativo](exemplos/hd_nativo_0.9.png) | ![Open-Scaling](exemplos/hd_open_scaling.png) |
| ![MSAA Nativo](exemplos/MSAA_hd_nativo.png) | ![MSAA Open Upscaling](exemplos/MSAA_open_upscaling.png) |

---

## 📄 Licença

Este projeto é distribuído sob a licença **MIT**.

Ele utiliza o **AMD FidelityFX Super Resolution 1.0 (FSR 1.0)**, que é disponibilizado pela AMD sob sua própria licença (MIT). Consulte o arquivo `LICENSE` e os avisos incluídos nos diretórios do FidelityFX para mais detalhes.

> **Aviso:** Este projeto **não é afiliado, endossado ou patrocinado pela AMD**. "AMD", "FidelityFX" e "FSR" são marcas registradas de Advanced Micro Devices, Inc.

---

## 🤝 Contribuição

Contribuições são bem-vindas! Para contribuir:

1. Faça um *fork* do projeto.
2. Crie uma branch: `git checkout -b feature/minha-feature`.
3. Commit suas mudanças: `git commit -m "feat: minha feature"`.
4. Envie para o fork: `git push origin feature/minha-feature`.
5. Abra um *Pull Request*.

Para reportar bugs ou sugerir melhorias, abra uma **issue** descrevendo:
- Sua distro e versão do kernel
- GPU e driver utilizado
- Passos para reproduzir o problema
- Logs e/ou screenshots, se possível

---

## 🔗 Links Úteis

- [AMD FidelityFX — FSR 1.0](https://github.com/GPUOpen-Effects/FidelityFX-FSR)
- [Vulkan SDK](https://vulkan.lunarg.com/)
- [Dear ImGui](https://github.com/ocornut/imgui)

---