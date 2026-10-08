// src/gui.cpp — Launcher grafico (Dear ImGui) que replica o menu do start.sh.
//
// Cria sua propria janela SDL2/Vulkan e reutiliza os helpers do projeto:
//   - capture_x11.c  -> lista de janelas, preview ao vivo, snapshot, bench
//   - vk_upscale.c   -> pipeline FSR 1.0 (EASU + RCAS) completo
//
// Compilar como C++ (usa imgui + backends SDL2/Vulkan).

#include "gui.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_vulkan.h"

// capture_x11.h / timer.h / vk_upscale.h sao C puro: sem extern "C" o linker
// procura nomes mangados em C++ e falha com "referencia nao definida".
extern "C" {
#include "capture_x11.h"
#include "timer.h"
}

#include <cstdarg>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <algorithm>
#include <fcntl.h>
#include <vector>
#include <string>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

// ------------------------------------------------------------------- terminal
// O launcher e apenas uma interface para comandar o binario em modo texto,
// exatamente como o start.sh faz. Cada acao vira um fork/exec de
// "./build/open-scaling <args>" cujo stdout/stderr cai no console da UI.
// Se for preciso voltar ao menu enquanto algo roda (FSR/preview), enviamos
// SIGTERM/SIGKILL — por isso ignoramos SIGINT/SIGQUIT aqui: Ctrl+C no
// terminal nao deve matar o launcher.
static void ignore_sigint() {
    struct sigaction sa = {};
    sa.sa_handler = SIG_IGN;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGQUIT, &sa, nullptr);
}

struct Proc {
    pid_t pid = -1;
    int   fd  = -1;   // leitura do stdout+stderr do filho
    char  label[96] = "";
    std::string partial;
};

extern "C" int vk_upscale_run(Window target, uint32_t out_w, uint32_t out_h,
                              float scale, uint32_t max_w, uint32_t max_h,
                              bool framegen_enabled, uint32_t framegen_factor,
                              const char *frameflow_socket, int requested_gpu);

struct VkGui {
    SDL_Window      *window   = nullptr;
    VkInstance       instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical  = VK_NULL_HANDLE;
    VkDevice         device    = VK_NULL_HANDLE;
    VkQueue          queue     = VK_NULL_HANDLE;
    uint32_t         qfam      = 0;
    VkSurfaceKHR     surface   = VK_NULL_HANDLE;

    VkSwapchainKHR   swapchain = VK_NULL_HANDLE;
    VkFormat         fmt       = VK_FORMAT_UNDEFINED;
    VkExtent2D       extent    = {};
    uint32_t         img_count = 0;
    std::vector<VkImage>     images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> fbss;

    VkRenderPass     renderpass = VK_NULL_HANDLE;
    VkPipelineCache  pipe_cache = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool  = VK_NULL_HANDLE;

    VkCommandPool    cmd_pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmds;

    std::vector<VkFence>     fences;
    std::vector<VkSemaphore> sem_avail;
    std::vector<VkSemaphore> sem_done;
    uint32_t frame = 0;

    // Estado da aplicacao (equivalente ao loop do start.sh)
    CaptureX11 cap       = {};
    bool       capturing = false;
    Window     target_id = 0;
    std::string target_title;
    Proc frameflow_proc;
    char frameflow_binary[512] = "../Open-FrameFlow/build/open-frameflow";
    char frameflow_model[512] = "";
    char frameflow_socket[108] = "";
    char frameflow_runtime_dir[128] = "";
    char frameflow_active_binary[512] = "";
    char frameflow_active_model[512] = "";
    int frameflow_gpu = 1;
    int frameflow_active_gpu = -1;
    int frameflow_algorithm = 0;
    int frameflow_active_algorithm = -1;
    int framegen_model_type = 0;
    int frameflow_active_model_type = -1;

    struct WinEntry { Window id; std::string title; int w; int h; };
    std::vector<WinEntry> windows;
    double last_scan = -1.0;

    char filter[128]   = "";
    char out_path[256] = "snap.ppm";
    char manual_id[64] = "";

    float scale = 1.5f;      // ultra 1.3 / quality 1.5 / balanced 1.7 / perf 2.0
    int   max_w = 1920;
    int   max_h = 1080;
    int   profile = 1;
    bool  framegen_enabled = false;
    int   framegen_factor = 0;

    bool  show_capture = true;
    bool  show_console = true;

    // Medicoes
    double fps_live = 0.0;
    double bench_fps = 0.0, bench_min = 0.0, bench_max = 0.0, bench_avg = 0.0;
    uint64_t bench_frames = 0;
    double bench_elapsed = 0.0;
    bool   bench_running = false;
    double bench_start = 0.0;
    double bench_seconds = 5.0;
    double bench_dur = 5.0;

    // Textura do preview
    struct Tex {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkBuffer stage = VK_NULL_HANDLE;
        VkDeviceMemory stage_mem = VK_NULL_HANDLE;
        void *stage_map = nullptr;
        size_t stage_size = 0;
        VkSampler sampler = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint32_t w = 0, h = 0;
    } tex;

    std::vector<std::string> log;
    bool quit = false;

    // "Terminal" embutido: o launcher comanda o binario via subprocessos,
    // como o start.sh faria no console.
    Proc   proc;
    time_t bin_mtime = 0;
    bool   readme_open = false;
};

// ---------------------------------------------------------------- utilitarios

static void gui_check(VkResult r, const char *what) {
    if (r == VK_SUCCESS) return;
    fprintf(stderr, "[gui] %s falhou: %d\n", what, (int)r);
    abort();
}
#define GUI_CHECK(x) gui_check((x), #x)

static void gui_log(VkGui *g, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g->log.emplace_back(buf);
    if (g->log.size() > 200) g->log.erase(g->log.begin());
    printf("[gui] %s\n", buf);
}

static uint32_t find_mem_type(VkGui *g, uint32_t mask, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g->physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mask & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    gui_log(g, "memoria adequada nao encontrada");
    return 0;
}

// ------------------------------------------------------- subprocessos (start.sh-like)

#define BIN_PATH "./build/open-scaling"

static bool bin_exists() {
    struct stat st;
    return stat(BIN_PATH, &st) == 0 && (st.st_mode & S_IXUSR);
}

static time_t bin_mtime() {
    struct stat st;
    return stat(BIN_PATH, &st) == 0 ? st.st_mtime : 0;
}

static void proc_pump(VkGui *g, Proc &p);   // adiante

// Lanca o binario em modo texto com os argumentos dados (equivalente ao
// run_bin do start.sh). O filho herda o ambiente; stdout/stderr viram um
// pipe que a gente drena no console da UI.
static bool spawn_process(VkGui *g, Proc &proc, const char *path,
                          const char *label, char *const argv[]) {
    if (proc.pid > 0) {
        gui_log(g, "ja existe '%s' rodando; encerre-a antes", proc.label);
        return false;
    }
    int pfd[2];
    if (pipe(pfd) != 0) { gui_log(g, "pipe() falhou"); return false; }
    const int read_flags = fcntl(pfd[0], F_GETFL, 0);
    if (read_flags < 0 || fcntl(pfd[0], F_SETFL, read_flags | O_NONBLOCK) < 0) {
        gui_log(g, "nao foi possivel tornar o pipe nao bloqueante");
        close(pfd[0]); close(pfd[1]);
        return false;
    }

    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]); close(pfd[1]);
        gui_log(g, "fork() falhou");
        return false;
    }
    if (pid == 0) {
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[0]); close(pfd[1]);
        char *full[64];
        int n = 0;
        full[n++] = (char *)path;
        for (; argv[n - 1] && n < 63; n++) full[n] = argv[n - 1];
        full[n] = nullptr;
        execv(path, full);
        _exit(127);
    }
    close(pfd[1]);
    proc.pid = pid;
    proc.fd = pfd[0];
    snprintf(proc.label, sizeof(proc.label), "%s", label);
    gui_log(g, "$ %s %s", path, label);
    proc_pump(g, proc);
    return true;
}

static bool spawn_bin(VkGui *g, const char *label, char *const argv[]) {
    if (g->proc.pid > 0) {
        gui_log(g, "ja existe '%s' rodando; encerre-a antes", g->proc.label);
        return false;
    }
    return spawn_process(g, g->proc, BIN_PATH, label, argv);
}

static bool spawn_cmd(VkGui *g, const char *label, const char *a = nullptr,
                      const char *b = nullptr, const char *c = nullptr,
                      const char *d = nullptr, const char *e = nullptr,
                      const char *f = nullptr, const char *h = nullptr,
                      const char *i = nullptr, const char *j = nullptr) {
    char *argv[10] = { (char *)a, (char *)b, (char *)c, (char *)d, (char *)e,
                       (char *)f, (char *)h, (char *)i, (char *)j, nullptr };
    argv[9] = nullptr;
    return spawn_bin(g, label, argv);
}

// Drena o pipe do filho sem bloquear a UI; linhas viram entradas do console.
static void proc_pump(VkGui *g, Proc &p) {
    if (p.pid <= 0) return;

    char buf[4096];
    for (;;) {
        ssize_t r = read(p.fd, buf, sizeof(buf));
        if (r > 0) {
            p.partial.append(buf, (size_t)r);
            size_t nl;
            while ((nl = p.partial.find('\n')) != std::string::npos) {
                std::string line = p.partial.substr(0, nl);
                p.partial.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                gui_log(g, "%s", line.c_str());
            }
            if (p.partial.size() > 8192) p.partial.clear();
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        break;   // EAGAIN ou EOF
    }

    int status = 0;
    pid_t w = waitpid(p.pid, &status, WNOHANG);
    if (w == p.pid) {
        if (!p.partial.empty()) {
            gui_log(g, "%s", p.partial.c_str());
            p.partial.clear();
        }
        if (WIFEXITED(status))
            gui_log(g, "'%s' terminou (codigo %d)%s", p.label, WEXITSTATUS(status),
                    WEXITSTATUS(status) ? " <- erro" : "");
        else if (WIFSIGNALED(status))
            gui_log(g, "'%s' foi encerrada por sinal %d", p.label, WTERMSIG(status));
        close(p.fd);
        p = Proc{};
    }
}

static void proc_kill(VkGui *g) {
    if (g->proc.pid <= 0) return;
    gui_log(g, "encerrando '%s'...", g->proc.label);
    kill(g->proc.pid, SIGTERM);
}

static void proc_force_kill(VkGui *g) {
    if (g->proc.pid <= 0) return;
    kill(g->proc.pid, SIGKILL);
}

static bool frameflow_socket_ready(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(address.sun_path)) {
        close(fd);
        return false;
    }
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    const bool connected =
        connect(fd, (sockaddr *)&address, sizeof(address)) == 0;
    close(fd);
    return connected;
}

static void stop_frameflow_service(VkGui *g) {
    Proc &proc = g->frameflow_proc;
    if (proc.pid > 0) {
        gui_log(g, "encerrando servico Open-FrameFlow...");
        if (kill(proc.pid, SIGTERM) != 0 && errno != ESRCH)
            gui_log(g, "falha ao enviar SIGTERM ao Open-FrameFlow: %s",
                    strerror(errno));
        int status = 0;
        pid_t result;
        do {
            result = waitpid(proc.pid, &status, 0);
        } while (result < 0 && errno == EINTR);
        if (result < 0 && errno != ECHILD)
            gui_log(g, "waitpid do Open-FrameFlow falhou: %s", strerror(errno));
        if (proc.fd >= 0) close(proc.fd);
        proc = Proc{};
    }
    if (g->frameflow_socket[0]) {
        unlink(g->frameflow_socket);
        g->frameflow_socket[0] = '\0';
    }
    if (g->frameflow_runtime_dir[0]) {
        rmdir(g->frameflow_runtime_dir);
        g->frameflow_runtime_dir[0] = '\0';
    }
    g->frameflow_active_binary[0] = '\0';
    g->frameflow_active_model[0] = '\0';
    g->frameflow_active_gpu = -1;
    g->frameflow_active_algorithm = -1;
    g->frameflow_active_model_type = -1;
}

static bool start_frameflow_service(VkGui *g) {
    if (g->frameflow_proc.pid > 0) {
        const bool same_settings =
            strcmp(g->frameflow_binary, g->frameflow_active_binary) == 0 &&
            strcmp(g->frameflow_model, g->frameflow_active_model) == 0 &&
            g->frameflow_gpu == g->frameflow_active_gpu &&
            g->frameflow_algorithm == g->frameflow_active_algorithm &&
            g->framegen_model_type == g->frameflow_active_model_type;
        if (same_settings && frameflow_socket_ready(g->frameflow_socket))
            return true;
        if (g->proc.pid > 0) {
            gui_log(g, "pare o FSR antes de trocar o algoritmo/modelo");
            return false;
        }
        stop_frameflow_service(g);
    }
    if (g->proc.pid > 0) {
        gui_log(g, "encerre '%s' antes de iniciar o servico", g->proc.label);
        return false;
    }
    if (access(g->frameflow_binary, X_OK) != 0) {
        gui_log(g, "Open-FrameFlow nao executavel: %s", g->frameflow_binary);
        return false;
    }
    if (g->frameflow_algorithm == 0) {
        if (!g->frameflow_model[0]) {
            gui_log(g, "informe a pasta do modelo RIFE");
            return false;
        }
        char model_file[sizeof(g->frameflow_model) + 32];
        snprintf(model_file, sizeof(model_file), "%s/flownet.param",
                 g->frameflow_model);
        if (access(model_file, R_OK) != 0) {
            gui_log(g, "flownet.param nao encontrado em %s", g->frameflow_model);
            return false;
        }
        snprintf(model_file, sizeof(model_file), "%s/flownet.bin",
                 g->frameflow_model);
        if (access(model_file, R_OK) != 0) {
            gui_log(g, "flownet.bin nao encontrado em %s", g->frameflow_model);
            return false;
        }
    }

    char runtime_template[] = "/tmp/open-frameflow-gui-XXXXXX";
    char *runtime_dir = mkdtemp(runtime_template);
    if (!runtime_dir) {
        gui_log(g, "mkdtemp para socket FrameFlow falhou: %s", strerror(errno));
        return false;
    }
    snprintf(g->frameflow_runtime_dir, sizeof(g->frameflow_runtime_dir),
             "%s", runtime_dir);
    snprintf(g->frameflow_socket, sizeof(g->frameflow_socket),
             "%s/frameflow.sock", runtime_dir);

    char gpu_id[16];
    snprintf(gpu_id, sizeof(gpu_id), "%d", g->frameflow_gpu);
    char *args[16];
    int n = 0;
    args[n++] = (char *)"--algorithm";
    args[n++] = (char *)(g->frameflow_algorithm == 0 ? "rife" : "dis");
    if (g->frameflow_algorithm == 0) {
        args[n++] = (char *)"--model";
        args[n++] = g->frameflow_model;
        args[n++] = (char *)"--gpu";
        args[n++] = gpu_id;
        args[n++] = (char *)"--inference-scale";
        args[n++] = (char *)"1.0";
        if (g->framegen_model_type == 1)
            args[n++] = (char *)"--require-int8";
    }
    args[n++] = (char *)"--socket";
    args[n++] = g->frameflow_socket;
    args[n] = nullptr;

    char label[192];
    if (g->frameflow_algorithm == 0) {
        snprintf(label, sizeof(label), "Open-FrameFlow (RIFE %s, Vulkan GPU %d)",
                 g->framegen_model_type == 1 ? "INT8" : "FP16",
                 g->frameflow_gpu);
    } else {
        snprintf(label, sizeof(label), "Open-FrameFlow (DIS CPU)");
    }
    if (!spawn_process(g, g->frameflow_proc, g->frameflow_binary, label, args)) {
        stop_frameflow_service(g);
        return false;
    }

    for (int i = 0; i < 600; ++i) {
        proc_pump(g, g->frameflow_proc);
        if (g->frameflow_proc.pid <= 0) {
            stop_frameflow_service(g);
            gui_log(g, "Open-FrameFlow encerrou durante a inicializacao");
            return false;
        }
        if (frameflow_socket_ready(g->frameflow_socket)) {
            snprintf(g->frameflow_active_binary,
                     sizeof(g->frameflow_active_binary), "%s",
                     g->frameflow_binary);
            snprintf(g->frameflow_active_model,
                     sizeof(g->frameflow_active_model), "%s",
                     g->frameflow_model);
            g->frameflow_active_gpu = g->frameflow_gpu;
            g->frameflow_active_algorithm = g->frameflow_algorithm;
            g->frameflow_active_model_type = g->framegen_model_type;
            gui_log(g, "Open-FrameFlow conectado pelo socket %s",
                    g->frameflow_socket);
            return true;
        }
        usleep(100000);
    }

    gui_log(g, "timeout aguardando o socket do Open-FrameFlow");
    stop_frameflow_service(g);
    return false;
}

// Executa um comando de shell (make, rm -rf, etc.) com saida no console.
static bool spawn_shell(VkGui *g, const char *label, const char *cmdline) {
    char *argv[4] = { (char *)"sh", (char *)"-c", (char *)cmdline, nullptr };
    return spawn_bin(g, label, argv);
}

// Compilar: Start-make.sh se existir, senao make (como build_project no start.sh)
static void run_build(VkGui *g) {
    if (access("./Start-make.sh", X_OK) == 0)
        spawn_shell(g, "Start-make.sh", "./Start-make.sh");
    else
        spawn_shell(g, "make", "make");
}

static void run_clean(VkGui *g) {
    spawn_shell(g, "limpar build", "rm -rf build && make clean 2>/dev/null; true");
}

// --------------------------------------------------------------- vulkan setup

static void init_instance(VkGui *g) {
    unsigned n = 0;
    SDL_Vulkan_GetInstanceExtensions(g->window, &n, nullptr);
    std::vector<const char *> exts(n);
    SDL_Vulkan_GetInstanceExtensions(g->window, &n, exts.data());

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "Open Scaling Launcher";
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = n;
    ci.ppEnabledExtensionNames = exts.data();
    GUI_CHECK(vkCreateInstance(&ci, nullptr, &g->instance));

    if (!SDL_Vulkan_CreateSurface(g->window, g->instance, &g->surface))
        gui_log(g, "SDL_Vulkan_CreateSurface falhou: %s", SDL_GetError());
}

static void pick_device(VkGui *g) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g->instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(g->instance, &n, devs.data());

    int best = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { best = (int)i; break; }
        if (best < 0 && p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) best = (int)i;
    }
    if (best < 0) best = 0;
    g->physical = devs[best];

    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(g->physical, &p);
    gui_log(g, "GPU: %s", p.deviceName);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g->physical, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qp(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(g->physical, &qn, qp.data());
    g->qfam = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++) {
        if (!(qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VkBool32 pres = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g->physical, i, g->surface, &pres);
        if (pres) { g->qfam = i; break; }
    }
    if (g->qfam == UINT32_MAX) gui_log(g, "nenhuma queue com suporte a surface");
}

static void create_device(VkGui *g) {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = g->qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    const char *exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo ci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = exts;
    GUI_CHECK(vkCreateDevice(g->physical, &ci, nullptr, &g->device));
    vkGetDeviceQueue(g->device, g->qfam, 0, &g->queue);

    VkPipelineCacheCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    GUI_CHECK(vkCreatePipelineCache(g->device, &pci, nullptr, &g->pipe_cache));
}

static void destroy_swapchain_objects(VkGui *g) {
    if (g->device) vkDeviceWaitIdle(g->device);
    for (VkFramebuffer fb : g->fbss) vkDestroyFramebuffer(g->device, fb, nullptr);
    g->fbss.clear();
    for (VkImageView v : g->views) vkDestroyImageView(g->device, v, nullptr);
    g->views.clear();
    g->images.clear();
    if (g->swapchain) { vkDestroySwapchainKHR(g->device, g->swapchain, nullptr); g->swapchain = VK_NULL_HANDLE; }
    if (g->renderpass) { vkDestroyRenderPass(g->device, g->renderpass, nullptr); g->renderpass = VK_NULL_HANDLE; }
    if (g->desc_pool) { vkDestroyDescriptorPool(g->device, g->desc_pool, nullptr); g->desc_pool = VK_NULL_HANDLE; }
    if (g->cmd_pool) { vkDestroyCommandPool(g->device, g->cmd_pool, nullptr); g->cmd_pool = VK_NULL_HANDLE; }
    for (VkFence f : g->fences) vkDestroyFence(g->device, f, nullptr);
    g->fences.clear();
    for (VkSemaphore s : g->sem_avail) vkDestroySemaphore(g->device, s, nullptr);
    g->sem_avail.clear();
    for (VkSemaphore s : g->sem_done) vkDestroySemaphore(g->device, s, nullptr);
    g->sem_done.clear();
    g->cmds.clear();
}

static void create_swapchain(VkGui *g, int width, int height) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g->physical, g->surface, &caps);

    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g->physical, g->surface, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g->physical, g->surface, &fn, fmts.data());
    VkSurfaceFormatKHR chosen = fmts.empty() ? VkSurfaceFormatKHR{} : fmts[0];
    for (auto &f : fmts) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
            chosen = f;
            break;
        }
    }
    g->fmt = chosen.format;

    g->extent = caps.currentExtent;
    if (g->extent.width == UINT32_MAX) {
        g->extent.width = (uint32_t)width;
        g->extent.height = (uint32_t)height;
    }

    uint32_t ic = caps.minImageCount + 1;
    if (caps.maxImageCount && ic > caps.maxImageCount) ic = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = g->surface;
    ci.minImageCount = ic;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = g->extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    GUI_CHECK(vkCreateSwapchainKHR(g->device, &ci, nullptr, &g->swapchain));

    vkGetSwapchainImagesKHR(g->device, g->swapchain, &g->img_count, nullptr);
    g->images.resize(g->img_count);
    vkGetSwapchainImagesKHR(g->device, g->swapchain, &g->img_count, g->images.data());
    g->views.resize(g->img_count);
    for (uint32_t i = 0; i < g->img_count; i++) {
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = g->images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = g->fmt;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        GUI_CHECK(vkCreateImageView(g->device, &vci, nullptr, &g->views[i]));
    }
}

static void create_renderpass(VkGui *g) {
    VkAttachmentDescription color = {};
    color.format = g->fmt;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    VkSubpassDependency dep = {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ci.attachmentCount = 1;
    ci.pAttachments = &color;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;
    GUI_CHECK(vkCreateRenderPass(g->device, &ci, nullptr, &g->renderpass));
}

static void create_framebuffers_and_cmds(VkGui *g) {
    g->fbss.resize(g->img_count);
    for (uint32_t i = 0; i < g->img_count; i++) {
        VkFramebufferCreateInfo ci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        ci.renderPass = g->renderpass;
        ci.attachmentCount = 1;
        ci.pAttachments = &g->views[i];
        ci.width = g->extent.width;
        ci.height = g->extent.height;
        ci.layers = 1;
        GUI_CHECK(vkCreateFramebuffer(g->device, &ci, nullptr, &g->fbss[i]));
    }

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g->qfam;
    GUI_CHECK(vkCreateCommandPool(g->device, &pci, nullptr, &g->cmd_pool));

    g->cmds.resize(g->img_count);
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = g->cmd_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = g->img_count;
    GUI_CHECK(vkAllocateCommandBuffers(g->device, &ai, g->cmds.data()));

    g->fences.resize(g->img_count);
    g->sem_avail.resize(g->img_count);
    g->sem_done.resize(g->img_count);
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < g->img_count; i++) {
        GUI_CHECK(vkCreateFence(g->device, &fci, nullptr, &g->fences[i]));
        GUI_CHECK(vkCreateSemaphore(g->device, &sci, nullptr, &g->sem_avail[i]));
        GUI_CHECK(vkCreateSemaphore(g->device, &sci, nullptr, &g->sem_done[i]));
    }
}

static void create_descriptor_pool(VkGui *g) {
    VkDescriptorPoolSize sizes[2] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32 },
    };
    VkDescriptorPoolCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    ci.maxSets = 128;
    ci.poolSizeCount = 2;
    ci.pPoolSizes = sizes;
    GUI_CHECK(vkCreateDescriptorPool(g->device, &ci, nullptr, &g->desc_pool));
}

static bool init_imgui(VkGui *g) {
    ImGui_ImplVulkan_InitInfo vi = {};
    vi.Instance = g->instance;
    vi.PhysicalDevice = g->physical;
    vi.Device = g->device;
    vi.QueueFamily = g->qfam;
    vi.Queue = g->queue;
    vi.DescriptorPool = g->desc_pool;
    vi.RenderPass = g->renderpass;
    vi.MinImageCount = 2;
    vi.ImageCount = g->img_count;
    vi.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    vi.PipelineCache = g->pipe_cache;
    vi.CheckVkResultFn = [](VkResult err) {
        if (err == VK_SUCCESS) return;
        fprintf(stderr, "[imgui-vk] erro: %d\n", (int)err);
        abort();
    };
    if (!ImGui_ImplVulkan_Init(&vi)) {
        fprintf(stderr, "[imgui-vk] falha ao inicializar backend Vulkan\n");
        return false;
    }

    // Fontes: sobem numa command buffer dedicada antes do primeiro frame.
    VkCommandBuffer upload;
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = g->cmd_pool;
    ai.commandBufferCount = 1;
    GUI_CHECK(vkAllocateCommandBuffers(g->device, &ai, &upload));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(upload, &bi);
    if (!ImGui_ImplVulkan_CreateFontsTexture()) {
        vkEndCommandBuffer(upload);   // evita command buffer aberta em caso de falha
        return false;
    }
    vkEndCommandBuffer(upload);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &upload;
    GUI_CHECK(vkQueueSubmit(g->queue, 1, &si, VK_NULL_HANDLE));
    GUI_CHECK(vkQueueWaitIdle(g->queue));
    ImGui_ImplVulkan_DestroyFontsTexture();
    vkFreeCommandBuffers(g->device, g->cmd_pool, 1, &upload);
    return true;
}

static void recreate_swapchain(VkGui *g, int w, int h) {
    // Ordem obrigatória: esperar a GPU -> desligar backend Vulkan (libera o
    // descriptor pool ANTES de ele ser destruído) -> destruir swapchain/pool.
    vkDeviceWaitIdle(g->device);
    ImGui_ImplVulkan_Shutdown();
    destroy_swapchain_objects(g);
    create_swapchain(g, w, h);
    create_renderpass(g);
    create_framebuffers_and_cmds(g);
    create_descriptor_pool(g);
    if (!init_imgui(g)) {
        fprintf(stderr, "[gui] falha ao reinicializar imgui apos resize\n");
        abort();
    }
    // Textura do preview vivia no descritor/sampler do ciclo anterior.
    g->tex = {};
}

// ------------------------------------------------------------------ lifecycle

static void gui_destroy(VkGui *g) {
    if (g->capturing) capture_shutdown(&g->cap);
    stop_frameflow_service(g);
    if (g->device) vkDeviceWaitIdle(g->device);
    // O descritor do preview vive no pool do backend: primeiro RemoveTexture,
    // depois Shutdown (que devolve o pool), só então destruir sampler/imagens.
    if (g->tex.set) { ImGui_ImplVulkan_RemoveTexture(g->tex.set); g->tex.set = VK_NULL_HANDLE; }
    if (g->tex.sampler) vkDestroySampler(g->device, g->tex.sampler, nullptr);
    if (g->tex.stage) vkDestroyBuffer(g->device, g->tex.stage, nullptr);
    if (g->tex.stage_mem) vkFreeMemory(g->device, g->tex.stage_mem, nullptr);
    if (g->tex.view) vkDestroyImageView(g->device, g->tex.view, nullptr);
    if (g->tex.image) vkDestroyImage(g->device, g->tex.image, nullptr);
    if (g->tex.mem) vkFreeMemory(g->device, g->tex.mem, nullptr);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    destroy_swapchain_objects(g);
    if (g->pipe_cache) vkDestroyPipelineCache(g->device, g->pipe_cache, nullptr);
    if (g->device) vkDestroyDevice(g->device, nullptr);
    if (g->surface) vkDestroySurfaceKHR(g->instance, g->surface, nullptr);
    if (g->instance) vkDestroyInstance(g->instance, nullptr);
}

// ------------------------------------------------------------- conteudo/menu

static void scan_windows(VkGui *g) {
    Display *dpy = XOpenDisplay(nullptr);
    g->windows.clear();
    if (!dpy) return;
    Window root = DefaultRootWindow(dpy), r, p, *children = nullptr;
    unsigned n = 0;
    if (XQueryTree(dpy, root, &r, &p, &children, &n)) {
        for (unsigned i = 0; i < n; i++) {
            XWindowAttributes a;
            if (!XGetWindowAttributes(dpy, children[i], &a)) continue;
            if (a.map_state != IsViewable || a.override_redirect) continue;
            if (a.width < 64 || a.height < 64) continue;
            char *name = nullptr;
            XFetchName(dpy, children[i], &name);
            VkGui::WinEntry we;
            we.id = children[i];
            we.title = name ? name : "(sem titulo)";
            we.w = a.width;
            we.h = a.height;
            if (name) XFree(name);
            g->windows.push_back(std::move(we));
        }
        XFree(children);
    }
    XCloseDisplay(dpy);
}

static void start_capture(VkGui *g, Window target) {
    if (g->capturing) { capture_shutdown(&g->cap); g->capturing = false; }
    if (!capture_init_target(&g->cap, target)) {
        gui_log(g, "falha ao iniciar captura de 0x%lx", (unsigned long)target);
        return;
    }
    g->capturing = true;
    g->target_id = g->cap.target;
    char hex[32];
    snprintf(hex, sizeof(hex), "0x%lx", (unsigned long)g->cap.target);
    g->target_title = hex;
    gui_log(g, "capturando %dx%d (%s)", g->cap.width, g->cap.height, hex);
}

// O binario precisa existir para qualquer acao do menu (como ensure_binary no
// start.sh). Aqui apenas avisamos e sugerimos o botao [6] — nao compilamos
// sozinhos para nao rodar um make surpresa em cima do build do usuario.
static bool ensure_binary(VkGui *g) {
    if (bin_exists()) return true;
    gui_log(g, "binario '%s' nao encontrado — use [6. Compilar] primeiro", BIN_PATH);
    return false;
}

static void stop_capture(VkGui *g) {
    if (!g->capturing) return;
    capture_shutdown(&g->cap);
    g->capturing = false;
    g->target_id = 0;
    gui_log(g, "captura parada");
}

static void ensure_texture(VkGui *g, uint32_t w, uint32_t h) {
    if (g->tex.w == w && g->tex.h == h && g->tex.image) return;
    vkDeviceWaitIdle(g->device);
    if (g->tex.set) { ImGui_ImplVulkan_RemoveTexture(g->tex.set); g->tex.set = VK_NULL_HANDLE; }
    if (g->tex.sampler) vkDestroySampler(g->device, g->tex.sampler, nullptr);
    if (g->tex.stage) vkDestroyBuffer(g->device, g->tex.stage, nullptr);
    if (g->tex.stage_mem) vkFreeMemory(g->device, g->tex.stage_mem, nullptr);
    if (g->tex.view) vkDestroyImageView(g->device, g->tex.view, nullptr);
    if (g->tex.image) vkDestroyImage(g->device, g->tex.image, nullptr);
    if (g->tex.mem) vkFreeMemory(g->device, g->tex.mem, nullptr);
    g->tex = {};
    g->tex.w = w;
    g->tex.h = h;

    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = { w, h, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    GUI_CHECK(vkCreateImage(g->device, &ici, nullptr, &g->tex.image));

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g->device, g->tex.image, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem_type(g, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    GUI_CHECK(vkAllocateMemory(g->device, &mai, nullptr, &g->tex.mem));
    vkBindImageMemory(g->device, g->tex.image, g->tex.mem, 0);

    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = g->tex.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    GUI_CHECK(vkCreateImageView(g->device, &vci, nullptr, &g->tex.view));

    g->tex.stage_size = (size_t)w * h * 4;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = g->tex.stage_size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    GUI_CHECK(vkCreateBuffer(g->device, &bci, nullptr, &g->tex.stage));
    vkGetBufferMemoryRequirements(g->device, g->tex.stage, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem_type(g, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    GUI_CHECK(vkAllocateMemory(g->device, &mai, nullptr, &g->tex.stage_mem));
    vkBindBufferMemory(g->device, g->tex.stage, g->tex.stage_mem, 0);
    GUI_CHECK(vkMapMemory(g->device, g->tex.stage_mem, 0, g->tex.stage_size, 0, &g->tex.stage_map));

    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    GUI_CHECK(vkCreateSampler(g->device, &sci, nullptr, &g->tex.sampler));

    g->tex.set = ImGui_ImplVulkan_AddTexture(g->tex.sampler, g->tex.view,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

static void update_preview(VkGui *g) {
    // Sem janela selecionada ou com um comando em execucao, nao capturamos:
    // o launcher vira apenas a interface que comanda o binario (start.sh-like).
    if (!g->capturing || !g->show_capture || g->proc.pid > 0) return;
    if (!capture_grab(&g->cap)) return;
    ensure_texture(g, (uint32_t)g->cap.width, (uint32_t)g->cap.height);

    unsigned char *src = capture_data(&g->cap);
    int src_stride = capture_stride(&g->cap);
    uint32_t *dst = (uint32_t *)g->tex.stage_map;
    for (int y = 0; y < g->cap.height; y++) {
        const uint32_t *srow = (const uint32_t *)(src + (size_t)y * src_stride);
        uint32_t *drow = dst + (size_t)y * g->cap.width;
        for (int x = 0; x < g->cap.width; x++) {
            uint32_t p = srow[x];   // BGRA -> RGBA
            drow[x] = ((p & 0xFFu) << 16) | (p & 0x00FF00u) |
                      ((p >> 16) & 0xFFu) | 0xFF000000u;
        }
    }

    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = g->cmd_pool;
    cai.commandBufferCount = 1;
    GUI_CHECK(vkAllocateCommandBuffers(g->device, &cai, &cmd));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier b1 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.srcQueueFamilyIndex = b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.image = g->tex.image;
    b1.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b1);

    VkBufferImageCopy region = {};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { g->tex.w, g->tex.h, 1 };
    vkCmdCopyBufferToImage(cmd, g->tex.stage, g->tex.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier b2 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b2.srcQueueFamilyIndex = b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.image = g->tex.image;
    b2.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b2);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    GUI_CHECK(vkQueueSubmit(g->queue, 1, &si, VK_NULL_HANDLE));
    GUI_CHECK(vkQueueWaitIdle(g->queue));
    vkFreeCommandBuffers(g->device, g->cmd_pool, 1, &cmd);
}

static void build_menu(VkGui *g) {
    ImGui::SetNextWindowSize(ImVec2(560, 620), ImGuiCond_FirstUseEver);
    ImGui::Begin("Open Scaling - Launcher");

    // -------------------------------------------------- Status (como no start.sh)
    ImGui::SeparatorText("Status");
    if (!bin_exists()) {
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.f), u8"\u25CF nao compilado");
        ImGui::SameLine();
        ImGui::Text("(%s)", BIN_PATH);
    } else {
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.f), u8"\u25CF binario pronto");
        ImGui::SameLine();
        ImGui::Text("(%s)", BIN_PATH);
    }
    if (g->proc.pid > 0) {
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), u8"\u25B6 rodando: %s", g->proc.label);
    }

    // -------------------------------------------------- Janela alvo (pick_window)
    ImGui::Spacing();
    ImGui::SeparatorText("Janela alvo");
    if (ImGui::Button("Escanear janelas")) scan_windows(g);
    if (g->last_scan < 0 || ImGui::GetTime() - g->last_scan > 2.0) {
        g->last_scan = ImGui::GetTime();
        scan_windows(g);
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "filtrar por titulo...", g->filter, sizeof(g->filter));
    if (ImGui::BeginListBox("##janelas", ImVec2(-1, 120))) {
        for (auto &we : g->windows) {
            if (*g->filter && !strstr(we.title.c_str(), g->filter)) continue;
            char label[256];
            snprintf(label, sizeof(label), "%s  (%dx%d)", we.title.c_str(), we.w, we.h);
            if (ImGui::Selectable(label, we.id == g->target_id)) {
                g->target_id = we.id;
                g->target_title = we.title;
                char hex[32];
                snprintf(hex, sizeof(hex), "0x%lx", (unsigned long)we.id);
                snprintf(g->manual_id, sizeof(g->manual_id), "%s", hex);
            }
        }
        ImGui::EndListBox();
    }
    ImGui::SetNextItemWidth(160);
    ImGui::InputText("ID manual (hex)", g->manual_id, sizeof(g->manual_id));
    ImGui::SameLine();
    if (ImGui::Button("Usar ID")) {
        unsigned long id = strtoul(g->manual_id, nullptr, 0);
        if (id) {
            g->target_id = (Window)id;
            g->target_title = "(ID manual)";
        } else {
            gui_log(g, "ID invalido: %s", g->manual_id);
        }
    }
    if (g->target_id) {
        ImGui::Text("Selecionada: %s (0x%lx)", g->target_title.c_str(),
                    (unsigned long)g->target_id);
    } else {
        ImGui::TextDisabled("Nenhuma janela selecionada.");
    }

    // -------------------------------------------------- Opcoes [1]..[9] do start.sh
    static const char *profiles[] = { "ultra (1.3x)", "quality (1.5x)",
                                      "balanced (1.7x)", "performance (2.0x)" };
    static const char *profile_names[4] = { "ultra", "quality", "balanced", "performance" };

    ImGui::Spacing();
    ImGui::SeparatorText("Acoes");

    // [1] Rodar FSR
    ImGui::Combo("Perfil", &g->profile, profiles, 4);
    ImGui::DragInt2("Resolucao maxima", &g->max_w, 16.f, 320, 3840, "%d");
    ImGui::Checkbox("Frame Generation (Open-FrameFlow)", &g->framegen_enabled);
    if (g->framegen_enabled) {
        static const char *framegen_factors[] = { "2x", "3x", "4x" };
        ImGui::Combo("Multiplicador de frames", &g->framegen_factor,
                     framegen_factors, 3);
        static const char *framegen_algorithms[] = {
            "RIFE (Vulkan)", "Optical Flow DIS (CPU)"
        };
        ImGui::Combo("Metodo de framegen", &g->frameflow_algorithm,
                     framegen_algorithms, 2);
        if (g->frameflow_algorithm == 0) {
            static const char *framegen_models[] = {
            "Original (FP16 storage)", "Modelo quantizado INT8"
            };
            ImGui::Combo("Precisao/modelo RIFE", &g->framegen_model_type,
                         framegen_models, 2);
        }
        ImGui::InputText("Executavel Open-FrameFlow", g->frameflow_binary,
                         sizeof(g->frameflow_binary));
        if (g->frameflow_algorithm == 0) {
            ImGui::InputText("Pasta do modelo RIFE", g->frameflow_model,
                             sizeof(g->frameflow_model));
            ImGui::InputInt("GPU Vulkan do RIFE", &g->frameflow_gpu);
            if (g->framegen_model_type == 1)
                ImGui::TextDisabled("Use uma pasta convertida pelo ncnn2int8; "
                                    "a opcao valida camadas INT8 no flownet.param.");
        } else {
            ImGui::TextDisabled("DIS calcula o fluxo na CPU; nao usa modelo RIFE.");
        }
        if (g->frameflow_proc.pid > 0) {
            if (g->frameflow_active_algorithm == 0)
                ImGui::Text("Servico FrameFlow ativo (RIFE, GPU %d).",
                            g->frameflow_active_gpu);
            else
                ImGui::Text("Servico FrameFlow ativo (DIS, CPU).");
            ImGui::SameLine();
            if (ImGui::Button("Parar servico FrameFlow"))
                stop_frameflow_service(g);
        }
    }
    if (ImGui::Button("Capturar para preview")) {
        if (!g->target_id) gui_log(g, "selecione uma janela primeiro");
        else start_capture(g, g->target_id);
    }
    ImGui::SameLine();
    if (ImGui::Button("Parar captura")) stop_capture(g);
    if (g->capturing) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.f), u8"\u25CF capturando %dx%d",
                           g->cap.width, g->cap.height);
    }

    if (ImGui::Button("1. Rodar FSR (upscale)")) {
        if (!g->target_id) {
            gui_log(g, "selecione uma janela primeiro");
        } else if (ensure_binary(g)) {
            char hex[32], maxres[32], factor[2], label[160];
            snprintf(hex, sizeof(hex), "0x%lx", (unsigned long)g->target_id);
            snprintf(maxres, sizeof(maxres), "%dx%d", g->max_w, g->max_h);
            if (g->framegen_enabled) {
                const int factor_value =
                    std::clamp(g->framegen_factor, 0, 2) + 2;
                snprintf(factor, sizeof(factor), "%d", factor_value);
                if (g->frameflow_algorithm == 0 && g->frameflow_gpu < 0) {
                    gui_log(g, "o indice da GPU Vulkan nao pode ser negativo");
                } else if (start_frameflow_service(g)) {
                    snprintf(label, sizeof(label),
                             "fsr %s %s --max %s --framegen %s",
                             hex, profile_names[g->profile], maxres, factor);
                    spawn_cmd(g, label, "fsr", hex, profile_names[g->profile],
                              "--max", maxres, "--framegen", factor,
                              "--frameflow-socket", g->frameflow_socket);
                }
            } else {
                snprintf(label, sizeof(label), "fsr %s %s --max %s",
                         hex, profile_names[g->profile], maxres);
                spawn_cmd(g, label, "fsr", hex, profile_names[g->profile],
                          "--max", maxres);
            }
        }
    }

    // [2] Preview
    ImGui::SameLine();
    if (ImGui::Button("2. Preview")) {
        if (ensure_binary(g)) spawn_cmd(g, "preview", "preview");
    }

    // [3] Benchmark
    static const double k_bench_min = 1.0, k_bench_max = 30.0;
    ImGui::SliderScalar("Duracao bench (s)", ImGuiDataType_Double, &g->bench_dur,
                        &k_bench_min, &k_bench_max, "%.0f");
    if (ImGui::Button("3. Benchmark")) {
        char dur[16], label[32];
        snprintf(dur, sizeof(dur), "%.0f", g->bench_dur);
        snprintf(label, sizeof(label), "bench %s", dur);
        if (ensure_binary(g)) spawn_cmd(g, label, "bench", dur);
    }

    // [4] Snapshot
    ImGui::SetNextItemWidth(220);
    ImGui::InputText("Arquivo", g->out_path, sizeof(g->out_path));
    ImGui::SameLine();
    if (ImGui::Button("4. Snapshot")) {
        char label[300];
        snprintf(label, sizeof(label), "snap %s", g->out_path);
        if (ensure_binary(g)) spawn_cmd(g, label, "snap", g->out_path);
    }

    // [5] Listar janelas — a lista acima ja e ao vivo; logamos um resumo.
    if (ImGui::Button("5. Listar janelas (log)")) {
        scan_windows(g);
        gui_log(g, "%zu janelas visiveis:", g->windows.size());
        for (auto &we : g->windows)
            gui_log(g, "  0x%lx  %s (%dx%d)", (unsigned long)we.id, we.title.c_str(), we.w, we.h);
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Manutencao");
    // [6] Compilar
    if (ImGui::Button("6. Compilar / Recompilar")) run_build(g);
    // [7] Limpar build
    ImGui::SameLine();
    if (ImGui::Button("7. Limpar build")) run_clean(g);
    // [8] README
    ImGui::SameLine();
    if (ImGui::Button("8. Ver README")) g->readme_open = true;
    // [9] Ajuda
    ImGui::SameLine();
    if (ImGui::Button("9. Ajuda (--help)")) {
        if (ensure_binary(g)) spawn_cmd(g, "--help", "--help");
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Visualizacao");
    ImGui::Checkbox("Mostrar preview da captura", &g->show_capture);
    ImGui::SameLine();
    ImGui::Checkbox("Mostrar console", &g->show_console);

    ImGui::Spacing();
    if (g->frameflow_proc.pid > 0) {
        if (ImGui::Button("Parar servico Open-FrameFlow"))
            stop_frameflow_service(g);
        ImGui::SameLine();
    }
    if (g->proc.pid > 0) {
        if (ImGui::Button("Encerrar processo")) proc_kill(g);
        ImGui::SameLine();
        if (ImGui::Button("Forcar SIGKILL")) proc_force_kill(g);
        ImGui::SameLine();
        if (ImGui::Button("Voltar ao menu (Sair)")) g->quit = true;
    } else {
        if (ImGui::Button("Sair")) g->quit = true;
    }
    ImGui::End();
}

static void build_preview(VkGui *g) {
    if (!g->show_capture || !g->capturing || !g->tex.set) return;
    ImGui::SetNextWindowSize(ImVec2(g->cap.width * 0.5f, g->cap.height * 0.5f + 40),
                             ImGuiCond_FirstUseEver);
    ImGui::Begin("Preview da captura", &g->show_capture);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float sc = fminf(avail.x / g->cap.width, avail.y / g->cap.height);
    if (sc > 0) {
        ImGui::Image((ImTextureID)(intptr_t)g->tex.set,
                     ImVec2(g->cap.width * sc, g->cap.height * sc));
    }
    ImGui::End();
}

static void build_console(VkGui *g) {
    if (!g->show_console) return;
    ImGui::SetNextWindowSize(ImVec2(560, 300), ImGuiCond_FirstUseEver);
    ImGui::Begin("Console", &g->show_console);
    ImGui::SeparatorText("Saida dos comandos");
    ImGui::BeginChild("scroll", ImVec2(0, 0), true);
    for (auto &line : g->log) ImGui::TextUnformatted(line.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

// Janela de leitura do README.md (equivalente ao show_readme/less do start.sh)
static void build_readme(VkGui *g) {
    if (!g->readme_open) return;
    ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("README.md", &g->readme_open)) {
        static std::vector<std::string> lines;
        static time_t loaded_at = 0;
        struct stat st;
        time_t readme_m = stat("README.md", &st) == 0 ? st.st_mtime : 0;
        if (lines.empty() || readme_m != loaded_at) {
            lines.clear();
            loaded_at = readme_m;
            FILE *fp = fopen("README.md", "rb");
            if (fp) {
                char buf[4096];
                std::string partial;
                while (fgets(buf, sizeof(buf), fp)) {
                    partial.append(buf);
                    size_t nl;
                    while ((nl = partial.find('\n')) != std::string::npos) {
                        lines.push_back(partial.substr(0, nl));
                        partial.erase(0, nl + 1);
                    }
                }
                if (!partial.empty()) lines.push_back(partial);
                fclose(fp);
            } else {
                lines.emplace_back("(README.md nao encontrado no diretorio do launcher)");
            }
        }
        ImGui::BeginChild("readme_scroll", ImVec2(0, 0), true);
        for (auto &l : lines) ImGui::TextUnformatted(l.c_str());
        ImGui::EndChild();
    }
    ImGui::End();
}

// -------------------------------------------------------------- frame principal

static bool process_events(VkGui *g) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        ImGui_ImplSDL2_ProcessEvent(&e);
        if (e.type == SDL_QUIT) g->quit = true;
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE &&
            !ImGui::GetIO().WantCaptureKeyboard)
            g->quit = true;
        if (e.type == SDL_WINDOWEVENT &&
            (e.window.event == SDL_WINDOWEVENT_RESIZED ||
             e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) &&
            e.window.data1 > 0 && e.window.data2 > 0) {
            recreate_swapchain(g, e.window.data1, e.window.data2);
        }
    }
    return !g->quit;
}

int vk_gui_run(void) {
    ignore_sigint();   // Ctrl+C no terminal nao deve fechar o launcher
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    VkGui *g = new VkGui();
    if (const char *binary = std::getenv("OPEN_FRAMEFLOW_BIN"))
        snprintf(g->frameflow_binary, sizeof(g->frameflow_binary), "%s", binary);
    if (const char *model = std::getenv("OPEN_FRAMEFLOW_MODEL"))
        snprintf(g->frameflow_model, sizeof(g->frameflow_model), "%s", model);
    else if (const char *home = std::getenv("HOME"))
        snprintf(g->frameflow_model, sizeof(g->frameflow_model),
                 "%s/Repo/librife-ncnn-vulkan/models/rife-v4", home);
    if (const char *gpu = std::getenv("OPEN_FRAMEFLOW_GPU"))
        g->frameflow_gpu = std::max(0, std::atoi(gpu));
    g->window = SDL_CreateWindow("Open Scaling - Launcher (Dear ImGui)",
                                 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!g->window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        delete g;
        return 1;
    }

    init_instance(g);
    pick_device(g);
    create_device(g);
    int w = 0, h = 0;
    SDL_Vulkan_GetDrawableSize(g->window, &w, &h);
    create_swapchain(g, w, h);
    create_renderpass(g);
    create_framebuffers_and_cmds(g);
    create_descriptor_pool(g);

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 6.0f;
    st.FrameRounding = 4.0f;
    if (!ImGui_ImplSDL2_InitForVulkan(g->window)) {
        fprintf(stderr, "[gui] falha ao inicializar backend SDL2\n");
        gui_destroy(g);
        SDL_DestroyWindow(g->window);
        SDL_Quit();
        delete g;
        return 1;
    }
    if (!init_imgui(g)) {
        fprintf(stderr, "[gui] falha ao inicializar Dear ImGui/Vulkan\n");
        gui_destroy(g);
        SDL_DestroyWindow(g->window);
        SDL_Quit();
        delete g;
        return 1;
    }

    gui_log(g, "launcher pronto");

    uint64_t frames = 0;
    double t0 = ImGui::GetTime();

    while (process_events(g)) {
        const pid_t command_pid = g->proc.pid;
        proc_pump(g, g->proc);
        if (command_pid > 0 && g->proc.pid == 0)
            stop_frameflow_service(g);
        const pid_t frameflow_pid = g->frameflow_proc.pid;
        proc_pump(g, g->frameflow_proc);
        if (frameflow_pid > 0 && g->frameflow_proc.pid == 0)
            stop_frameflow_service(g);
        update_preview(g);

        frames++;
        double now = ImGui::GetTime();
        if (now - t0 >= 1.0) { g->fps_live = frames / (now - t0); frames = 0; t0 = now; }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        build_menu(g);
        build_preview(g);
        build_console(g);
        build_readme(g);
        ImGui::Render();

        vkWaitForFences(g->device, 1, &g->fences[g->frame], VK_TRUE, UINT64_MAX);
        uint32_t ii;
        VkResult r = vkAcquireNextImageKHR(g->device, g->swapchain, UINT64_MAX,
                                           g->sem_avail[g->frame], VK_NULL_HANDLE, &ii);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
            vkResetFences(g->device, 1, &g->fences[g->frame]);
            int dw, dh;
            SDL_Vulkan_GetDrawableSize(g->window, &dw, &dh);
            recreate_swapchain(g, dw, dh);
            continue;
        }
        vkResetFences(g->device, 1, &g->fences[g->frame]);

        VkCommandBuffer cmd = g->cmds[ii];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        vkBeginCommandBuffer(cmd, &bi);

        VkClearValue clear = {};
        clear.color = { { 0.08f, 0.09f, 0.10f, 1.0f } };
        VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rp.renderPass = g->renderpass;
        rp.framebuffer = g->fbss[ii];
        rp.renderArea = { {0,0}, g->extent };
        rp.clearValueCount = 1;
        rp.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        vkCmdEndRenderPass(cmd);
        GUI_CHECK(vkEndCommandBuffer(cmd));

        VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &g->sem_avail[g->frame];
        si.pWaitDstStageMask = &wait;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &g->sem_done[g->frame];
        GUI_CHECK(vkQueueSubmit(g->queue, 1, &si, g->fences[g->frame]));

        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &g->sem_done[g->frame];
        pi.swapchainCount = 1;
        pi.pSwapchains = &g->swapchain;
        pi.pImageIndices = &ii;
        vkQueuePresentKHR(g->queue, &pi);

        g->frame = (g->frame + 1) % g->img_count;
    }

    gui_destroy(g);
    SDL_DestroyWindow(g->window);
    SDL_Quit();
    delete g;
    return 0;
}
