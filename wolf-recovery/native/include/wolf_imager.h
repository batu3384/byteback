#pragma once

#include "wolf_io.h"
#include "imager/ewf_writer.h"
#include <string>
#include <atomic>
#include <thread>
#include <functional>
#include <memory>

namespace wolf {

enum class ImageFormat {
    Raw, // plain dd-style byte-for-byte image
    Ewf  // Expert Witness Format (.E01) with on-the-fly MD5
};

class DiskImager {
public:
    // Callback receives (currentSector, totalSectors); when imaging finishes,
    // an extra call with current == total signals completion and the
    // progress.data string in NAPI carries the final MD5 for EWF images.
    using ProgressCallback = std::function<void(uint64_t currentSector, uint64_t totalSectors)>;

    DiskImager();
    ~DiskImager();

    void startImaging(int driveIndex, const std::string& destPath, ProgressCallback onProgress,
                      ImageFormat format = ImageFormat::Raw);
    void stopImaging();

    // MD5 hex of the imaged data; valid after imaging completes (EWF only,
    // computed on the fly for both formats but only surfaced for EWF).
    std::string lastImageMd5() const { return lastImageMd5_; }

    void setReverseImaging(bool reverse);
    void setMaxRetries(int retries);

private:
    void imagingWorker(int driveIndex, std::string destPath, ProgressCallback onProgress, ImageFormat format);

    std::atomic<bool> isRunning_;
    std::thread imagingThread_;

    std::atomic<bool> reverseImaging_{false};
    std::atomic<int> maxRetries_{3};
    std::string lastImageMd5_;
};

} // namespace wolf
