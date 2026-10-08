#include "frameflow_client.h"
#include "frameflow_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define FRAMEFLOW_OUTPUT_SLOTS 12

struct FrameFlowClient {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int socket_fd;
    uint32_t width;
    uint32_t height;
    uint32_t factor;
    size_t frame_bytes;
    size_t raw_frame_bytes;
    unsigned char *pending;
    unsigned char *submit_scratch;
    unsigned char *reading;
    unsigned char *outputs[FRAMEFLOW_OUTPUT_SLOTS];
    uint64_t output_sequences[FRAMEFLOW_OUTPUT_SLOTS];
    uint32_t output_steps[FRAMEFLOW_OUTPUT_SLOTS];
    uint64_t submitted_sequence;
    uint64_t pending_sequence;
    uint32_t output_head;
    uint32_t output_count;
    bool has_pending;
    bool stop;
    bool failed;
};

static bool transfer_all(int fd, void *buffer, size_t size, bool writing) {
    unsigned char *bytes = buffer;
    size_t offset = 0;
    while (offset < size) {
        ssize_t count;
        if (writing)
            count = send(fd, bytes + offset, size - offset, MSG_NOSIGNAL);
        else
            count = recv(fd, bytes + offset, size - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += (size_t)count;
    }
    return true;
}

static void rgb_from_bgra(const unsigned char *src, unsigned char *dst,
                          uint32_t width, uint32_t height) {
    size_t pixels = (size_t)width * height;
    for (size_t i = 0; i < pixels; ++i) {
        dst[i * 3] = src[i * 4 + 2];
        dst[i * 3 + 1] = src[i * 4 + 1];
        dst[i * 3 + 2] = src[i * 4];
    }
}

static bool exchange_pair(FrameFlowClient *client,
                          const unsigned char *first, const unsigned char *second,
                          uint64_t sequence, unsigned char *rgb_input0,
                          unsigned char *rgb_input1, unsigned char *rgb_output) {
    rgb_from_bgra(first, rgb_input0, client->width, client->height);
    rgb_from_bgra(second, rgb_input1, client->width, client->height);

    uint32_t request[FRAMEFLOW_HEADER_WORDS] = {
        FRAMEFLOW_MAGIC, FRAMEFLOW_VERSION, client->width, client->height,
        client->factor, FRAMEFLOW_RGB24, (uint32_t)client->frame_bytes,
        (uint32_t)(sequence >> 32), (uint32_t)sequence, 0
    };
    for (uint32_t i = 0; i < FRAMEFLOW_HEADER_WORDS; ++i)
        request[i] = htonl(request[i]);

    if (!transfer_all(client->socket_fd, request, sizeof(request), true) ||
        !transfer_all(client->socket_fd, rgb_input0, client->frame_bytes, true) ||
        !transfer_all(client->socket_fd, rgb_input1, client->frame_bytes, true))
        return false;

    uint32_t response[FRAMEFLOW_HEADER_WORDS];
    if (!transfer_all(client->socket_fd, response, sizeof(response), false))
    {
        fprintf(stderr,
                "[frameflow] conexao fechada antes do cabecalho de resposta (seq=%llu).\n",
                (unsigned long long)sequence);
        return false;
    }
    for (uint32_t i = 0; i < FRAMEFLOW_HEADER_WORDS; ++i)
        response[i] = ntohl(response[i]);

    if (response[FRAMEFLOW_RES_MAGIC] != FRAMEFLOW_MAGIC ||
        response[FRAMEFLOW_RES_VERSION] != FRAMEFLOW_VERSION ||
        response[FRAMEFLOW_RES_STATUS] != FRAMEFLOW_STATUS_OK ||
        response[FRAMEFLOW_RES_WIDTH] != client->width ||
        response[FRAMEFLOW_RES_HEIGHT] != client->height ||
        response[FRAMEFLOW_RES_FACTOR] != client->factor ||
        response[FRAMEFLOW_RES_FRAME_BYTES] != client->frame_bytes ||
        response[FRAMEFLOW_RES_SEQUENCE_HI] != (uint32_t)(sequence >> 32) ||
        response[FRAMEFLOW_RES_SEQUENCE_LO] != (uint32_t)sequence) {
        fprintf(stderr,
                "[frameflow] resposta invalida seq=%u:%u: magic=0x%08x versao=%u status=%u tamanho=%ux%u fator=%u bytes=%u seq=%u:%u.\n",
                (uint32_t)(sequence >> 32), (uint32_t)sequence,
                response[FRAMEFLOW_RES_MAGIC], response[FRAMEFLOW_RES_VERSION],
                response[FRAMEFLOW_RES_STATUS], response[FRAMEFLOW_RES_WIDTH],
                response[FRAMEFLOW_RES_HEIGHT], response[FRAMEFLOW_RES_FACTOR],
                response[FRAMEFLOW_RES_FRAME_BYTES],
                response[FRAMEFLOW_RES_SEQUENCE_HI],
                response[FRAMEFLOW_RES_SEQUENCE_LO]);
        return false;
    }

    for (uint32_t i = 0; i < client->factor - 1; ++i) {
        if (!transfer_all(client->socket_fd, rgb_output, client->frame_bytes, false)) {
            fprintf(stderr,
                    "[frameflow] conexao fechada lendo saida seq=%llu passo=%u.\n",
                    (unsigned long long)sequence, i + 1);
            return false;
        }

        pthread_mutex_lock(&client->mutex);
        while (client->output_count == FRAMEFLOW_OUTPUT_SLOTS &&
               !client->stop && !client->failed)
            pthread_cond_wait(&client->cond, &client->mutex);
        if (client->stop || client->failed) {
            pthread_mutex_unlock(&client->mutex);
            return false;
        }
        const uint32_t slot =
            (client->output_head + client->output_count) % FRAMEFLOW_OUTPUT_SLOTS;
        memcpy(client->outputs[slot], rgb_output, client->frame_bytes);
        client->output_sequences[slot] = sequence;
        client->output_steps[slot] = i + 1;
        client->output_count++;
        pthread_cond_broadcast(&client->cond);
        pthread_mutex_unlock(&client->mutex);
    }
    return true;
}

static void *frameflow_worker(void *userdata) {
    FrameFlowClient *client = userdata;
    unsigned char *previous = malloc(client->raw_frame_bytes);
    unsigned char *current = malloc(client->raw_frame_bytes);
    unsigned char *rgb_input0 = malloc(client->frame_bytes);
    unsigned char *rgb_input1 = malloc(client->frame_bytes);
    unsigned char *rgb_output = malloc(client->frame_bytes);
    if (!previous || !current || !rgb_input0 || !rgb_input1 || !rgb_output) {
        fprintf(stderr, "[frameflow] sem memoria para os buffers de IPC.\n");
        goto failed;
    }

    bool have_previous = false;
    uint64_t previous_sequence = 0;
    for (;;) {
        pthread_mutex_lock(&client->mutex);
        while (!client->has_pending && !client->stop)
            pthread_cond_wait(&client->cond, &client->mutex);
        if (client->stop) {
            pthread_mutex_unlock(&client->mutex);
            break;
        }
        unsigned char *free_current = current;
        current = client->pending;
        client->pending = free_current;
        const uint64_t sequence = client->pending_sequence;
        client->has_pending = false;
        pthread_cond_broadcast(&client->cond);
        pthread_mutex_unlock(&client->mutex);

        if (have_previous && sequence > previous_sequence &&
            !exchange_pair(client, previous, current, sequence, rgb_input0,
                           rgb_input1, rgb_output)) {
            pthread_mutex_lock(&client->mutex);
            const bool stopping = client->stop;
            pthread_mutex_unlock(&client->mutex);
            if (stopping) break;
            fprintf(stderr, "[frameflow] servico desconectado ou protocolo invalido; frame generation desativado.\n");
            goto failed;
        }
        unsigned char *old_previous = previous;
        previous = current;
        current = old_previous;
        previous_sequence = sequence;
        have_previous = true;
    }
    free(previous);
    free(current);
    free(rgb_input0);
    free(rgb_input1);
    free(rgb_output);
    return NULL;

failed:
    free(previous);
    free(current);
    free(rgb_input0);
    free(rgb_input1);
    free(rgb_output);
    pthread_mutex_lock(&client->mutex);
    client->failed = true;
    pthread_cond_broadcast(&client->cond);
    pthread_mutex_unlock(&client->mutex);
    return NULL;
}

FrameFlowClient *frameflow_client_start(const char *socket_path,
                                        uint32_t width, uint32_t height,
                                        uint32_t factor) {
    if (!socket_path || !width || !height ||
        factor < FRAMEFLOW_MIN_FACTOR || factor > FRAMEFLOW_MAX_FACTOR ||
        (uint64_t)width * height > FRAMEFLOW_MAX_PIXELS) {
        fprintf(stderr, "[frameflow] parametros invalidos; recurso desativado.\n");
        return NULL;
    }

    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(socket_path) >= sizeof(address.sun_path)) {
        fprintf(stderr, "[frameflow] caminho do socket excede o limite; recurso desativado.\n");
        return NULL;
    }
    strcpy(address.sun_path, socket_path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("[frameflow] socket");
        return NULL;
    }
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        fprintf(stderr, "[frameflow] servico indisponivel em %s; seguindo sem frame generation.\n",
                socket_path);
        close(fd);
        return NULL;
    }

    FrameFlowClient *client = calloc(1, sizeof(*client));
    if (!client) {
        perror("[frameflow] calloc");
        close(fd);
        return NULL;
    }
    client->socket_fd = fd;
    client->width = width;
    client->height = height;
    client->factor = factor;
    const size_t pixels = (size_t)width * height;
    client->frame_bytes = pixels * 3;
    client->raw_frame_bytes = pixels * 4;
    client->pending = malloc(client->raw_frame_bytes);
    client->submit_scratch = malloc(client->raw_frame_bytes);
    client->reading = malloc(client->frame_bytes);
    for (uint32_t i = 0; i < FRAMEFLOW_OUTPUT_SLOTS; ++i)
        client->outputs[i] = malloc(client->frame_bytes);
    if (!client->pending || !client->submit_scratch || !client->reading ||
        pthread_mutex_init(&client->mutex, NULL) != 0) {
        fprintf(stderr, "[frameflow] falha ao alocar/inicializar cliente.\n");
        goto init_failed;
    }
    if (pthread_cond_init(&client->cond, NULL) != 0) {
        fprintf(stderr, "[frameflow] falha ao inicializar condicao do cliente.\n");
        pthread_mutex_destroy(&client->mutex);
        goto init_failed;
    }
    for (uint32_t i = 0; i < FRAMEFLOW_OUTPUT_SLOTS; ++i) {
        if (!client->outputs[i]) {
            fprintf(stderr, "[frameflow] falha ao alocar fila de saida.\n");
            pthread_cond_destroy(&client->cond);
            pthread_mutex_destroy(&client->mutex);
            goto init_failed;
        }
    }
    if (pthread_create(&client->thread, NULL, frameflow_worker, client) != 0) {
        fprintf(stderr, "[frameflow] falha ao iniciar thread do cliente.\n");
        pthread_cond_destroy(&client->cond);
        pthread_mutex_destroy(&client->mutex);
        goto init_failed;
    }
    fprintf(stderr, "[frameflow] conectado; multiplicador %ux.\n", factor);
    return client;

init_failed:
    free(client->pending);
    free(client->submit_scratch);
    free(client->reading);
    for (uint32_t i = 0; i < FRAMEFLOW_OUTPUT_SLOTS; ++i)
        free(client->outputs[i]);
    close(client->socket_fd);
    free(client);
    return NULL;
}

void frameflow_client_submit(FrameFlowClient *client,
                             const unsigned char *bgra, int stride) {
    if (!client || !bgra || stride < 0 ||
        (size_t)stride < (size_t)client->width * 4)
        return;
    for (uint32_t y = 0; y < client->height; ++y)
        memcpy(client->submit_scratch + (size_t)y * client->width * 4,
               bgra + (size_t)y * stride, (size_t)client->width * 4);

    pthread_mutex_lock(&client->mutex);
    if (!client->stop && !client->failed) {
        unsigned char *available = client->pending;
        client->pending = client->submit_scratch;
        client->submit_scratch = available;
        client->pending_sequence = ++client->submitted_sequence;
        client->has_pending = true;
        pthread_cond_signal(&client->cond);
    }
    pthread_mutex_unlock(&client->mutex);
}

uint64_t frameflow_client_submit_ordered(FrameFlowClient *client,
                                         const unsigned char *bgra, int stride) {
    if (!client || !bgra || stride < 0 ||
        (size_t)stride < (size_t)client->width * 4)
        return 0;

    pthread_mutex_lock(&client->mutex);
    while (client->has_pending && !client->stop && !client->failed)
        pthread_cond_wait(&client->cond, &client->mutex);
    if (client->stop || client->failed) {
        pthread_mutex_unlock(&client->mutex);
        return 0;
    }

    for (uint32_t y = 0; y < client->height; ++y)
        memcpy(client->pending + (size_t)y * client->width * 4,
               bgra + (size_t)y * stride, (size_t)client->width * 4);
    const uint64_t sequence = ++client->submitted_sequence;
    client->pending_sequence = sequence;
    client->has_pending = true;
    pthread_cond_signal(&client->cond);
    pthread_mutex_unlock(&client->mutex);
    return sequence;
}

static void rgb_to_rgba_flipped(FrameFlowClient *client, const unsigned char *rgb,
                                unsigned char *rgba) {
    for (uint32_t y = 0; y < client->height; ++y) {
        const unsigned char *src = rgb + (size_t)y * client->width * 3;
        unsigned char *dst = rgba +
            (size_t)(client->height - 1 - y) * client->width * 4;
        for (uint32_t x = 0; x < client->width; ++x) {
            dst[x * 4] = src[x * 3];
            dst[x * 4 + 1] = src[x * 3 + 1];
            dst[x * 4 + 2] = src[x * 3 + 2];
            dst[x * 4 + 3] = 255;
        }
    }
}

static unsigned char *take_output_locked(FrameFlowClient *client, uint32_t slot) {
    unsigned char *rgb = client->outputs[slot];
    client->outputs[slot] = client->reading;
    client->reading = rgb;
    client->output_head = (client->output_head + 1) % FRAMEFLOW_OUTPUT_SLOTS;
    client->output_count--;
    pthread_cond_broadcast(&client->cond);
    return rgb;
}

bool frameflow_client_take_latest(FrameFlowClient *client,
                                  unsigned char *rgba) {
    if (!client || !rgba) return false;
    pthread_mutex_lock(&client->mutex);
    if (!client->output_count) {
        pthread_mutex_unlock(&client->mutex);
        return false;
    }
    const uint32_t slot = client->output_head;
    unsigned char *rgb = take_output_locked(client, slot);
    pthread_mutex_unlock(&client->mutex);

    rgb_to_rgba_flipped(client, rgb, rgba);
    return true;
}

bool frameflow_client_take_sequence(FrameFlowClient *client,
                                    uint64_t sequence, uint32_t step,
                                    unsigned char *rgba) {
    if (!client || !sequence || !step || !rgba) return false;
    pthread_mutex_lock(&client->mutex);
    while (client->output_count == 0 && !client->stop && !client->failed)
        pthread_cond_wait(&client->cond, &client->mutex);

    if (client->output_count == 0) {
        pthread_mutex_unlock(&client->mutex);
        return false;
    }
    const uint32_t slot = client->output_head;
    if (client->output_sequences[slot] != sequence ||
        client->output_steps[slot] != step) {
        fprintf(stderr,
                "[frameflow] ordem de saida inesperada: recebida seq=%llu passo=%u, esperada seq=%llu passo=%u.\n",
                (unsigned long long)client->output_sequences[slot],
                client->output_steps[slot], (unsigned long long)sequence, step);
        pthread_mutex_unlock(&client->mutex);
        return false;
    }
    unsigned char *rgb = take_output_locked(client, slot);
    pthread_mutex_unlock(&client->mutex);
    rgb_to_rgba_flipped(client, rgb, rgba);
    return true;
}

bool frameflow_client_is_active(FrameFlowClient *client) {
    if (!client) return false;
    pthread_mutex_lock(&client->mutex);
    const bool active = !client->stop && !client->failed;
    pthread_mutex_unlock(&client->mutex);
    return active;
}

void frameflow_client_request_stop(FrameFlowClient *client) {
    if (!client) return;
    pthread_mutex_lock(&client->mutex);
    client->stop = true;
    pthread_cond_broadcast(&client->cond);
    pthread_mutex_unlock(&client->mutex);
    shutdown(client->socket_fd, SHUT_RDWR);
}

void frameflow_client_stop(FrameFlowClient *client) {
    if (!client) return;
    frameflow_client_request_stop(client);
    pthread_join(client->thread, NULL);
    pthread_cond_destroy(&client->cond);
    pthread_mutex_destroy(&client->mutex);
    close(client->socket_fd);
    free(client->pending);
    free(client->submit_scratch);
    free(client->reading);
    for (uint32_t i = 0; i < FRAMEFLOW_OUTPUT_SLOTS; ++i)
        free(client->outputs[i]);
    free(client);
}
