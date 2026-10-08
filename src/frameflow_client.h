#ifndef OPEN_SCALING_FRAMEFLOW_CLIENT_H
#define OPEN_SCALING_FRAMEFLOW_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct FrameFlowClient FrameFlowClient;

FrameFlowClient *frameflow_client_start(const char *socket_path,
                                        uint32_t width, uint32_t height,
                                        uint32_t factor);
void frameflow_client_submit(FrameFlowClient *client,
                             const unsigned char *bgra, int stride);
uint64_t frameflow_client_submit_ordered(FrameFlowClient *client,
                                         const unsigned char *bgra, int stride);
bool frameflow_client_take_latest(FrameFlowClient *client,
                                  unsigned char *rgba);
bool frameflow_client_take_sequence(FrameFlowClient *client,
                                    uint64_t sequence, uint32_t step,
                                    unsigned char *rgba);
bool frameflow_client_is_active(FrameFlowClient *client);
void frameflow_client_request_stop(FrameFlowClient *client);
void frameflow_client_stop(FrameFlowClient *client);

#endif
