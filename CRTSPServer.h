#pragma once

// CRTSPServer - header-only RTSP server wrapper.
// CMake usage (header-only):
//   target_sources(main PRIVATE CRTSPServer.h)  // optional, for IDEs
//   target_include_directories(main PRIVATE ${OpenCV_INCLUDE_DIRS} ${GST_INCLUDE_DIRS})
//   target_link_libraries(main PRIVATE ${OpenCV_LIBS} ${GST_LIBRARIES})

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <gst/app/gstappsrc.h>

#include <opencv2/opencv.hpp>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

class CRTSPServer {
public:
    static constexpr int kWidth = 1280;
    static constexpr int kHeight = 720;
    static constexpr int kFps = 25;

    CRTSPServer() = default;
    ~CRTSPServer();

    bool Init(int argc, char *argv[]);
    void Push(const cv::Mat &frame);

private:
    static void MediaConfigure(GstRTSPMediaFactory *factory,
                               GstRTSPMedia *media,
                               gpointer user_data);
    static void MediaUnprepared(GstRTSPMedia *media, gpointer user_data);

    void OnMediaConfigure(GstRTSPMedia *media);
    void OnMediaUnprepared(GstRTSPMedia *media);
    void MainLoop();

    std::mutex appsrc_mutex_;
    GstElement *appsrc_ = nullptr;
    std::atomic<bool> client_connected_{false};
    std::atomic<bool> reset_timestamps_{false};

    std::thread main_loop_thread_;
    GMainLoop *main_loop_ = nullptr;
    GstRTSPServer *server_ = nullptr;
    GstRTSPMediaFactory *factory_ = nullptr;
    guint server_id_ = 0;

    int width_ = kWidth;
    int height_ = kHeight;
    int fps_ = kFps;
    GstClockTime frame_duration_ = 0;
    GstClockTime pts_ = 0;
    int frame_index_ = 0;
    cv::Mat yuv_;
};

inline CRTSPServer::~CRTSPServer()
{
    if (main_loop_) {
        g_main_loop_quit(main_loop_);
    }
    if (main_loop_thread_.joinable()) {
        main_loop_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(appsrc_mutex_);
        if (appsrc_) {
            gst_object_unref(appsrc_);
            appsrc_ = nullptr;
        }
    }
    if (server_id_ != 0) {
        g_source_remove(server_id_);
        server_id_ = 0;
    }
    if (server_) {
        g_object_unref(server_);
        server_ = nullptr;
    }
    if (factory_) {
        g_object_unref(factory_);
        factory_ = nullptr;
    }
    if (main_loop_) {
        g_main_loop_unref(main_loop_);
        main_loop_ = nullptr;
    }
}

inline bool CRTSPServer::Init(int argc, char *argv[])
{
    gst_init(&argc, &argv);

    frame_duration_ = gst_util_uint64_scale_int(1, GST_SECOND, fps_);
    pts_ = 0;
    frame_index_ = 0;

    server_ = gst_rtsp_server_new();
    if (!server_) {
        return false;
    }
    gst_rtsp_server_set_service(server_, "8554");

    GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(server_);
    if (factory_) {
        g_object_unref(factory_);
        factory_ = nullptr;
    }
    factory_ = gst_rtsp_media_factory_new();
    if (!factory_) {
        g_object_unref(mounts);
        return false;
    }

    std::string launch =
        "( appsrc name=src is-live=true format=time "
        "caps=video/x-raw,format=I420,width=" + std::to_string(width_) +
        ",height=" + std::to_string(height_) +
        ",framerate=" + std::to_string(fps_) + "/1 "
        "! videoconvert "
        "! x264enc tune=zerolatency speed-preset=ultrafast bitrate=2000 "
        "! rtph264pay name=pay0 pt=96 )";

    gst_rtsp_media_factory_set_launch(factory_, launch.c_str());
    gst_rtsp_media_factory_set_shared(factory_, FALSE);
    g_signal_connect(factory_, "media-configure",
                     (GCallback)CRTSPServer::MediaConfigure, this);

    gst_rtsp_mount_points_add_factory(mounts, "/main", factory_);
    g_object_unref(mounts);

    server_id_ = gst_rtsp_server_attach(server_, nullptr);
    if (server_id_ == 0) {
        g_object_unref(server_);
        server_ = nullptr;
        g_object_unref(factory_);
        factory_ = nullptr;
        return false;
    }

    main_loop_ = g_main_loop_new(nullptr, FALSE);
    if (!main_loop_) {
        if (server_id_ != 0) {
            g_source_remove(server_id_);
            server_id_ = 0;
        }
        g_object_unref(server_);
        server_ = nullptr;
        g_object_unref(factory_);
        factory_ = nullptr;
        return false;
    }

    main_loop_thread_ = std::thread(&CRTSPServer::MainLoop, this);

    g_print("RTSP server running ip:8554/main\n");
    return true;
}

inline void CRTSPServer::Push(const cv::Mat &frame)
{
    GstElement *local_appsrc = nullptr;
    {
        std::lock_guard<std::mutex> lock(appsrc_mutex_);
        if (appsrc_) {
            local_appsrc = GST_ELEMENT(gst_object_ref(appsrc_));
        }
    }

    if (!client_connected_ || !local_appsrc) {
        if (local_appsrc) {
            gst_object_unref(local_appsrc);
        }
        return;
    }

    if (reset_timestamps_.exchange(false)) {
        pts_ = 0;
        frame_index_ = 0;
    }

    if (frame.empty()) {
        gst_object_unref(local_appsrc);
        return;
    }

    cv::Mat resized;
    const cv::Mat *source = &frame;
    if (frame.cols != width_ || frame.rows != height_) {
        cv::resize(frame, resized, cv::Size(width_, height_));
        source = &resized;
    }

    cv::cvtColor(*source, yuv_, cv::COLOR_BGR2YUV_I420);

    const gsize yuv_size = yuv_.total() * yuv_.elemSize();
    GstBuffer *buffer =
        gst_buffer_new_allocate(nullptr, yuv_size, nullptr);
    if (!buffer) {
        gst_object_unref(local_appsrc);
        return;
    }

    gst_buffer_fill(buffer, 0, yuv_.data, yuv_size);

    GST_BUFFER_PTS(buffer) = pts_;
    GST_BUFFER_DURATION(buffer) = frame_duration_;
    pts_ += frame_duration_;
    frame_index_++;

    GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(local_appsrc), buffer);
    if (flow != GST_FLOW_OK) {
        std::lock_guard<std::mutex> lock(appsrc_mutex_);
        if (appsrc_ == local_appsrc) {
            gst_object_unref(appsrc_);
            appsrc_ = nullptr;
            client_connected_ = false;
        }
    }

    gst_object_unref(local_appsrc);
}

inline void CRTSPServer::MediaConfigure(GstRTSPMediaFactory *,
                                        GstRTSPMedia *media,
                                        gpointer user_data)
{
    static_cast<CRTSPServer *>(user_data)->OnMediaConfigure(media);
}

inline void CRTSPServer::MediaUnprepared(GstRTSPMedia *media, gpointer user_data)
{
    static_cast<CRTSPServer *>(user_data)->OnMediaUnprepared(media);
}

inline void CRTSPServer::OnMediaConfigure(GstRTSPMedia *media)
{
    GstElement *pipeline = gst_rtsp_media_get_element(media);
    GstElement *new_appsrc = gst_bin_get_by_name_recurse_up(GST_BIN(pipeline), "src");

    g_object_set(new_appsrc,
        "is-live", TRUE,
        "format", GST_FORMAT_TIME,
        "do-timestamp", TRUE,
        nullptr
    );

    {
        std::lock_guard<std::mutex> lock(appsrc_mutex_);
        if (appsrc_) {
            gst_object_unref(appsrc_);
        }
        appsrc_ = new_appsrc;
    }

    client_connected_ = true;
    reset_timestamps_ = true;
    g_print("RTSP client connected\n");

    g_signal_connect(media, "unprepared",
                     (GCallback)CRTSPServer::MediaUnprepared, this);

    gst_object_unref(pipeline);
}

inline void CRTSPServer::OnMediaUnprepared(GstRTSPMedia *media)
{
    GstElement *pipeline = gst_rtsp_media_get_element(media);
    GstElement *src = gst_bin_get_by_name_recurse_up(GST_BIN(pipeline), "src");
    bool cleared = false;

    {
        std::lock_guard<std::mutex> lock(appsrc_mutex_);
        if (appsrc_ == src) {
            gst_object_unref(appsrc_);
            appsrc_ = nullptr;
            client_connected_ = false;
            cleared = true;
        }
    }

    gst_object_unref(src);
    gst_object_unref(pipeline);

    if (cleared) {
        g_print("RTSP client disconnected\n");
    }
}

inline void CRTSPServer::MainLoop()
{
    if (main_loop_) {
        g_main_loop_run(main_loop_);
    }
}
