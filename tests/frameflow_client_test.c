#include "frameflow_client.h"
#include "frameflow_protocol.h"

#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define TEST_WIDTH 2
#define TEST_HEIGHT 2
#define TEST_FACTOR 3
#define TEST_RGB_BYTES (TEST_WIDTH * TEST_HEIGHT * 3)
#define TEST_RGBA_BYTES (TEST_WIDTH * TEST_HEIGHT * 4)

static const uint32_t width = TEST_WIDTH;
static const uint32_t height = TEST_HEIGHT;
static const uint32_t factor = TEST_FACTOR;
static char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static bool request_valid;
static pthread_mutex_t listener_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t listener_cond = PTHREAD_COND_INITIALIZER;
static bool listener_ready;

static bool transfer_all(int fd, void *buffer, size_t size, bool writing) {
    unsigned char *bytes = buffer;
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = writing
            ? send(fd, bytes + offset, size - offset, 0)
            : recv(fd, bytes + offset, size - offset, 0);
        if (count <= 0) return false;
        offset += (size_t)count;
    }
    return true;
}

static void *mock_server(void *unused) {
    (void)unused;
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(server >= 0);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    strcpy(address.sun_path, socket_path);
    assert(bind(server, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(server, 1) == 0);
    pthread_mutex_lock(&listener_mutex);
    listener_ready = true;
    pthread_cond_signal(&listener_cond);
    pthread_mutex_unlock(&listener_mutex);
    int client = accept(server, NULL, NULL);
    assert(client >= 0);

    uint32_t request[FRAMEFLOW_HEADER_WORDS];
    assert(transfer_all(client, request, sizeof(request), false));
    for (uint32_t i = 0; i < FRAMEFLOW_HEADER_WORDS; ++i)
        request[i] = ntohl(request[i]);

    unsigned char input0[TEST_RGB_BYTES];
    unsigned char input1[TEST_RGB_BYTES];
    assert(transfer_all(client, input0, sizeof(input0), false));
    assert(transfer_all(client, input1, sizeof(input1), false));
    const unsigned char expected0[] = {
        3, 2, 1, 6, 5, 4, 9, 8, 7, 12, 11, 10
    };
    const unsigned char expected1[] = {
        33, 32, 31, 36, 35, 34, 39, 38, 37, 42, 41, 40
    };
    request_valid =
        request[FRAMEFLOW_REQ_MAGIC] == FRAMEFLOW_MAGIC &&
        request[FRAMEFLOW_REQ_VERSION] == FRAMEFLOW_VERSION &&
        request[FRAMEFLOW_REQ_WIDTH] == width &&
        request[FRAMEFLOW_REQ_HEIGHT] == height &&
        request[FRAMEFLOW_REQ_FACTOR] == factor &&
        request[FRAMEFLOW_REQ_FORMAT] == FRAMEFLOW_RGB24 &&
        request[FRAMEFLOW_REQ_FRAME_BYTES] == sizeof(input0) &&
        request[FRAMEFLOW_REQ_SEQUENCE_HI] == 0 &&
        request[FRAMEFLOW_REQ_SEQUENCE_LO] == 2 &&
        memcmp(input0, expected0, sizeof(input0)) == 0 &&
        memcmp(input1, expected1, sizeof(input1)) == 0;

    uint32_t response[FRAMEFLOW_HEADER_WORDS] = {
        FRAMEFLOW_MAGIC, FRAMEFLOW_VERSION, FRAMEFLOW_STATUS_OK,
        width, height, factor, sizeof(input0), 0, 2, 0
    };
    for (uint32_t i = 0; i < FRAMEFLOW_HEADER_WORDS; ++i)
        response[i] = htonl(response[i]);
    const unsigned char outputs[TEST_FACTOR - 1][TEST_RGB_BYTES] = {
        { 10, 11, 12, 20, 21, 22, 30, 31, 32, 40, 41, 42 },
        { 50, 51, 52, 60, 61, 62, 70, 71, 72, 80, 81, 82 }
    };
    assert(transfer_all(client, response, sizeof(response), true));
    assert(transfer_all(client, (void *)outputs, sizeof(outputs), true));
    close(client);
    close(server);
    return NULL;
}

static bool wait_for_sequence(FrameFlowClient *client, uint64_t sequence,
                              uint32_t step, unsigned char *rgba) {
    return frameflow_client_take_sequence(client, sequence, step, rgba);
}

static int live_test(const char *path) {
    const uint32_t live_width = 64;
    const uint32_t live_height = 64;
    const size_t live_size = (size_t)live_width * live_height * 4;
    FrameFlowClient *client = frameflow_client_start(
        path, live_width, live_height, 2);
    if (!client) return 1;

    unsigned char *frame0 = malloc(live_size);
    unsigned char *frame1 = malloc(live_size);
    unsigned char *rgba = malloc(live_size);
    assert(frame0 && frame1 && rgba);
    for (size_t i = 0; i < live_size; i += 4) {
        frame0[i] = 24; frame0[i + 1] = 48; frame0[i + 2] = 72; frame0[i + 3] = 255;
        frame1[i] = 96; frame1[i + 1] = 128; frame1[i + 2] = 160; frame1[i + 3] = 255;
    }

    assert(frameflow_client_submit_ordered(client, frame0, live_width * 4) == 1);
    assert(frameflow_client_submit_ordered(client, frame1, live_width * 4) == 2);
    const bool got_output = wait_for_sequence(client, 2, 1, rgba);
    frameflow_client_stop(client);
    assert(got_output);
    for (size_t i = 3; i < live_size; i += 4)
        assert(rgba[i] == 255);

    free(frame0);
    free(frame1);
    free(rgba);
    puts("Live Open-FrameFlow client test passed");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--live") == 0)
        return live_test(argv[2]);
    assert(argc == 1);
    snprintf(socket_path, sizeof(socket_path), "/tmp/open-frameflow-test-%ld.sock",
             (long)getpid());
    pthread_t server_thread;
    assert(pthread_create(&server_thread, NULL, mock_server, NULL) == 0);
    pthread_mutex_lock(&listener_mutex);
    while (!listener_ready) pthread_cond_wait(&listener_cond, &listener_mutex);
    pthread_mutex_unlock(&listener_mutex);

    FrameFlowClient *client = frameflow_client_start(
        socket_path, width, height, factor);
    assert(client);

    unsigned char frame0[TEST_RGBA_BYTES] = {
        1, 2, 3, 255, 4, 5, 6, 255,
        7, 8, 9, 255, 10, 11, 12, 255
    };
    unsigned char frame1[TEST_RGBA_BYTES] = {
        31, 32, 33, 255, 34, 35, 36, 255,
        37, 38, 39, 255, 40, 41, 42, 255
    };
    assert(frameflow_client_submit_ordered(client, frame0, width * 4) == 1);
    assert(frameflow_client_submit_ordered(client, frame1, width * 4) == 2);

    unsigned char rgba[TEST_RGBA_BYTES];
    const unsigned char expected_first[] = {
        30, 31, 32, 255, 40, 41, 42, 255,
        10, 11, 12, 255, 20, 21, 22, 255
    };
    const unsigned char expected_second[] = {
        70, 71, 72, 255, 80, 81, 82, 255,
        50, 51, 52, 255, 60, 61, 62, 255
    };
    assert(wait_for_sequence(client, 2, 1, rgba));
    assert(memcmp(rgba, expected_first, sizeof(rgba)) == 0);
    assert(wait_for_sequence(client, 2, 2, rgba));
    assert(memcmp(rgba, expected_second, sizeof(rgba)) == 0);

    frameflow_client_stop(client);
    assert(pthread_join(server_thread, NULL) == 0);
    unlink(socket_path);
    assert(request_valid);
    puts("FrameFlow client IPC test passed");
    return 0;
}
