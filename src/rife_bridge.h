#ifndef RIFE_BRIDGE_H
#define RIFE_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char  model_dir[512];
    int   gpuid;
    bool  initialized;
    void *rife;   // RIFE* opaco — só o .cpp mexe
} RifeBridge;

bool rife_bridge_init(RifeBridge* rb, const char* model_dir, int gpuid);
void rife_bridge_shutdown(RifeBridge* rb);

bool rife_bridge_interpolate(RifeBridge* rb,
                             const uint8_t* rgbA,
                             const uint8_t* rgbB,
                             int w, int h,
                             uint8_t* rgbOut);

#ifdef __cplusplus
}
#endif

#endif