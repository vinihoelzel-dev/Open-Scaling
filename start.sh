#!/bin/bash
# start.sh — Launcher para Open-Scaling (FSR 1.0 via Vulkan)
# Estilo visual inspirado no roleplay.sh, sem IA, só o programa.

# --- Cores ---
GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

# --- Vai para o diretório do script ---
cd "$(dirname "$0")" || exit 1

# --- Configuração central (ajuste aqui se mudar no repo) ---
BIN="./build/open-scaling"
BUILD_DIR="build"
SHADER_BUILD="./Start-make.sh"

# =========================================================
# Checagens
# =========================================================
check_tools() {
    local missing=0
    for tool in gcc make pkg-config; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo -e "${RED}❌ '$tool' não encontrado no PATH.${NC}"
            missing=1
        fi
    done
    if ! pkg-config --exists vulkan 2>/dev/null && [ ! -d "/usr/include/vulkan" ]; then
        echo -e "${YELLOW}⚠️  Vulkan headers/libs não detectados.${NC}"
    fi
    if ! pkg-config --exists sdl2 2>/dev/null; then
        echo -e "${YELLOW}⚠️  SDL2 não detectado. Instale 'libsdl2-dev'.${NC}"
    fi
    if ! pkg-config --exists x11 2>/dev/null; then
        echo -e "${YELLOW}⚠️  X11 não detectado. Instale 'libx11-dev'.${NC}"
    fi
    if ! command -v wmctrl >/dev/null 2>&1 && ! command -v xwininfo >/dev/null 2>&1; then
        echo -e "${YELLOW}⚠️  Nem 'wmctrl' nem 'xwininfo' disponíveis.${NC}"
        echo -e "${YELLOW}   Instale 'wmctrl' ou 'x11-utils' para listar janelas.${NC}"
    fi
    if [ "$missing" -eq 1 ]; then
        echo -e "${RED}Abortando: faltam ferramentas de build essenciais.${NC}"
        exit 1
    fi
}

# =========================================================
# Build / Binário
# =========================================================
build_project() {
    echo -e "${GREEN}🔨 Compilando Open-Scaling...${NC}"
    if [ -x "$SHADER_BUILD" ]; then
        "$SHADER_BUILD" || { echo -e "${RED}❌ Falha em $SHADER_BUILD.${NC}"; return 1; }
    elif [ -f "Makefile" ]; then
        make || { echo -e "${RED}❌ Falha ao rodar 'make'.${NC}"; return 1; }
    else
        echo -e "${RED}❌ Nem $SHADER_BUILD nem Makefile encontrados.${NC}"
        return 1
    fi
    if [ -x "$BIN" ]; then
        echo -e "${GREEN}✅ Build concluído: $BIN${NC}"
    else
        echo -e "${YELLOW}⚠️  Build terminou mas '$BIN' não existe.${NC}"
    fi
}

ensure_binary() {
    if [ ! -x "$BIN" ]; then
        echo -e "${YELLOW}⚠️  Binário '$BIN' não encontrado.${NC}"
        read -rp "Compilar agora? [S/n] " ans
        case "$ans" in
            n|N|no|No|NO) return 1 ;;
            *) build_project || return 1 ;;
        esac
    fi
    return 0
}

run_bin() {
    ensure_binary || return 1
    "$BIN" "$@"
    local rc=$?
    if [ $rc -ne 0 ]; then
        echo -e "${RED}❌ '$BIN $*' retornou código $rc.${NC}"
    fi
    return $rc
}

# =========================================================
# Leitura de janelas X11
# =========================================================
# Saída: linhas "ID|Título", uma por janela.
list_windows_raw() {
    if command -v wmctrl >/dev/null 2>&1; then
        wmctrl -l | while IFS=' ' read -r id _ _ rest; do
            [ -n "$id" ] && printf '%s|%s\n' "$id" "$rest"
        done
        return 0
    fi
    if command -v xwininfo >/dev/null 2>&1; then
        xwininfo -root -tree 2>/dev/null \
            | sed -nE 's/^[[:space:]]*(0x[0-9a-fA-F]+)[[:space:]]+"([^"]*)".*/\1|\2/p'
        return 0
    fi
    return 1
}

# Lista formatada + escolha. Devolve o ID escolhido no stdout.
pick_window() {
    local -a ids=()
    local -a titles=()

    while IFS='|' read -r id title; do
        [ -z "$id" ] && continue
        ids+=("$id")
        titles+=("${title:-<sem título>}")
    done < <(list_windows_raw)

    if [ "${#ids[@]}" -eq 0 ]; then
        echo -e "${RED}❌ Nenhuma janela encontrada.${NC}" >&2
        echo -e "${YELLOW}   Instale 'wmctrl' ou 'x11-utils' (xwininfo).${NC}" >&2
        return 1
    fi

    echo -e "${CYAN}Janelas disponíveis:${NC}" >&2
    local i=1
    for idx in "${!ids[@]}"; do
        printf "  ${YELLOW}[%2d]${NC} %-40.40s ${CYAN}(%s)${NC}\n" \
            "$i" "${titles[$idx]}" "${ids[$idx]}" >&2
        ((i++))
    done
    echo "" >&2

    read -rp "$(echo -e "${BOLD}Escolha o número (ou cole o ID direto, ENTER cancela): ${NC}")" sel
    [ -z "$sel" ] && return 1

    if [[ "$sel" =~ ^[0-9]+$ ]]; then
        local n=$((sel - 1))
        if [ "$n" -ge 0 ] && [ "$n" -lt "${#ids[@]}" ]; then
            printf '%s\n' "${ids[$n]}"
            return 0
        fi
        echo -e "${RED}❌ Índice inválido.${NC}" >&2
        return 1
    fi

    # Assume que é o ID (hex ou decimal)
    printf '%s\n' "$sel"
    return 0
}

# Lista "pura", usada pela opção [5] do menu.
show_windows() {
    echo -e "${GREEN}🪟 Janelas X11 disponíveis:${NC}"
    local found=0
    while IFS='|' read -r id title; do
        [ -z "$id" ] && continue
        printf "  ${CYAN}%s${NC}  %s\n" "$id" "${title:-<sem título>}"
        found=1
    done < <(list_windows_raw)
    if [ "$found" -eq 0 ]; then
        echo -e "${YELLOW}⚠️  Nenhuma janela listada.${NC}"
        echo -e "${YELLOW}   Instale 'wmctrl' ou 'x11-utils'.${NC}"
    fi
}

# =========================================================
# Utilitários de manutenção
# =========================================================
clean_build() {
    echo -e "${YELLOW}🧹 Limpando build...${NC}"
    if [ -d "$BUILD_DIR" ]; then
        rm -rf "$BUILD_DIR"
        echo -e "${GREEN}✅ '$BUILD_DIR' removido.${NC}"
    else
        echo -e "${YELLOW}⚠️  '$BUILD_DIR' não existe.${NC}"
    fi
    if [ -f "Makefile" ]; then
        make clean 2>/dev/null && echo -e "${GREEN}✅ make clean ok.${NC}"
    fi
}

show_readme() {
    if [ -f "README.md" ]; then
        ${PAGER:-less} README.md
    else
        echo -e "${YELLOW}⚠️  README.md não encontrado.${NC}"
    fi
}

# =========================================================
# Bootstrap
# =========================================================
clear
check_tools

# =========================================================
# Loop principal
# =========================================================
while true; do
    clear
    echo -e "${CYAN}======================================================${NC}"
    echo -e "${CYAN}   🎮 ${BOLD}OPEN-SCALING - LAUNCHER${NC}"
    echo -e "${CYAN}   🖥️  FSR 1.0 (EASU + RCAS) via Vulkan | X11${NC}"
    echo -e "${CYAN}======================================================${NC}"
    if [ -x "$BIN" ]; then
        echo -e "  Status: ${GREEN}● binário pronto${NC}  ($BIN)"
    else
        echo -e "  Status: ${RED}● não compilado${NC}"
    fi
    echo -e "${CYAN}------------------------------------------------------${NC}"
    echo -e "${YELLOW}[1]${NC} Rodar FSR (upscale com janela de preview)"
    echo -e "${YELLOW}[2]${NC} Preview (sem upscale, só visualizar captura)"
    echo -e "${YELLOW}[3]${NC} Benchmark"
    echo -e "${YELLOW}[4]${NC} Snapshot (capturar 1 frame de uma janela)"
    echo -e "${YELLOW}[5]${NC} Listar janelas disponíveis"
    echo -e "------------------------------------------------------"
    echo -e "${YELLOW}[6]${NC} Compilar / Recompilar"
    echo -e "${YELLOW}[7]${NC} Limpar build"
    echo -e "${YELLOW}[8]${NC} Ver README"
    echo -e "${YELLOW}[9]${NC} Ajuda do binário (--help)"
    echo -e "------------------------------------------------------"
    echo -e "${RED}[0]${NC} Sair"
    echo -e "${CYAN}======================================================${NC}"
    echo ""
    read -rp "$(echo -e "${BOLD}${YELLOW}Escolha uma opção:${NC} ")" choice

    case "$choice" in
        1)
            echo -e "${GREEN}🚀 Modo FSR${NC}"
            target=$(pick_window) || { read -rp "ENTER..."; continue; }
            read -rp "Escala (ex: 1.5) [enter = padrão]: " scale
            read -rp "Largura máx [enter = padrão]: " maxw
            read -rp "Altura máx [enter = padrão]: " maxh
            args=(fsr "$target")
            [ -n "$scale" ] && args+=(--scale "$scale")
            [ -n "$maxw" ]  && args+=(--max-w "$maxw")
            [ -n "$maxh" ]  && args+=(--max-h "$maxh")
            run_bin "${args[@]}"
            ;;
        2)
            echo -e "${GREEN}👁️  Modo Preview${NC}"
            target=$(pick_window) || { read -rp "ENTER..."; continue; }
            run_bin preview "$target"
            ;;
        3)
            echo -e "${GREEN}⏱️  Benchmark${NC}"
            target=$(pick_window) || { read -rp "ENTER..."; continue; }
            read -rp "Duração em segundos [enter = padrão]: " dur
            args=(bench "$target")
            [ -n "$dur" ] && args+=(--duration "$dur")
            run_bin "${args[@]}"
            ;;
        4)
            echo -e "${GREEN}📸 Snapshot${NC}"
            target=$(pick_window) || { read -rp "ENTER..."; continue; }
            read -rp "Arquivo de saída [snap.png]: " out
            out="${out:-snap.png}"
            run_bin snap "$target" --output "$out"
            ;;
        5)
            show_windows
            ;;
        6)
            build_project
            ;;
        7)
            clean_build
            ;;
        8)
            show_readme
            ;;
        9)
            ensure_binary && "$BIN" --help
            ;;
        0)
            echo -e "${GREEN}👋 Até logo!${NC}"
            exit 0
            ;;
        *)
            echo -e "${RED}❌ Opção inválida.${NC}"
            sleep 1
            continue
            ;;
    esac

    echo ""
    read -rp "Pressione ENTER para voltar ao menu..."
done