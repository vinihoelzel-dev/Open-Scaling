#include "rife_bridge.h"
#include "rife.h"
#include "net.h"

#include <cstdio>
#include <cstring>

extern "C" {

bool rife_bridge_init(RifeBridge* rb, const char* model_dir, int gpuid) {
    std::memset(rb, 0, sizeof(*rb));
    std::snprintf(rb->model_dir, sizeof(rb->model_dir), "%s", model_dir);
    rb->gpuid = gpuid;

    // Mesma assinatura do test_rife.cpp (que funciona):
    // RIFE(gpuid, tta, tta_temporal, uhd, num_threads, rife_v2, rife_v4)
    RIFE* rife = new RIFE(gpuid, false, false, false, 1, false, true);

    if (rife->load(rb->model_dir) != 0) {
        std::fprintf(stderr, "[rife_bridge] load falhou: %s\n", rb->model_dir);
        delete rife;
        return false;
    }

    rb->rife = rife;
    rb->initialized = true;
    std::printf("[rife_bridge] OK: model=%s gpu=%d\n", rb->model_dir, rb->gpuid);
    return true;
}

void rife_bridge_shutdown(RifeBridge* rb) {
    if (!rb->initialized) return;
    delete static_cast<RIFE*>(rb->rife);
    rb->rife = nullptr;
    rb->initialized = false;
}

bool rife_bridge_interpolate(RifeBridge* rb,
                             const uint8_t* rgbA,
                             const uint8_t* rgbB,
                             int w, int h,
                             uint8_t* rgbOut) {
    if (!rb->initialized) return false;
    RIFE* rife = static_cast<RIFE*>(rb->rife);

    ncnn::Mat in0 = ncnn::Mat::from_pixels(rgbA, ncnn::Mat::PIXEL_RGB, w, h);
    ncnn::Mat in1 = ncnn::Mat::from_pixels(rgbB, ncnn::Mat::PIXEL_RGB, w, h);
    ncnn::Mat out;

    if (rife->process(in0, in1, 0.5f, out) != 0) {
        std::fprintf(stderr, "[rife_bridge] process falhou\n");
        return false;
    }

    out.to_pixels(rgbOut, ncnn::Mat::PIXEL_RGB);
    return true;
}

} // extern "C"