#pragma once

#include <openmedia/hw_cuda.h>
#include <openmedia/video.hpp>
#include "nv_loader.hpp"

namespace openmedia {

class CudaHardwarePictureImpl : public CudaHardwarePicture {
    OMCudaPicture om_pic_;
public:
    explicit CudaHardwarePictureImpl() {
        om_pic_.data = 0;
        om_pic_.pitch = 0;
    }

    void updateOM() {
        om_pic_.data = data;
        om_pic_.pitch = pitch;
    }

    auto getOMPicture() -> OMCudaPicture* override { 
        updateOM();
        return &om_pic_; 
    }
};


} // namespace openmedia
