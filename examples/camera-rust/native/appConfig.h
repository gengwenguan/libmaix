#pragma once

#include <mutex>

// In-process config facade for selected native camera modules. Rust owns
// validation and persistence; this class only publishes the hot-path subset.
class C_AppConfig {
public:
    struct Snapshot {
        bool ai_enabled = false;
        float ai_threshold = 0.5f;
        int ai_min_interval_s = 3;
        int ai_infer_fps = 1;
        int album_max_photos = 1000;
        int photo_jpeg_qual = 88;
        int mic_filter_mode = 5;
        bool osd_show_ip = true;
        bool osd_show_time = true;
        bool osd_show_ai_box = true;
    };

    static C_AppConfig& GetInst()
    {
        static C_AppConfig instance;
        return instance;
    }

    Snapshot GetSnapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

    void SetSnapshot(const Snapshot& snapshot)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = snapshot;
    }

private:
    C_AppConfig() = default;
    mutable std::mutex mutex_;
    Snapshot snapshot_;
};
