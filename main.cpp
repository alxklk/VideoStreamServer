#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <gst/app/gstappsrc.h>

#include <opencv2/opencv.hpp>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

static GstElement *appsrc = nullptr;
static std::atomic<bool> client_connected{false};
static std::atomic<bool> reset_timestamps{false};
static std::mutex appsrc_mutex;

static void media_unprepared(GstRTSPMedia *media, gpointer)
{
    GstElement *pipeline = gst_rtsp_media_get_element(media);
    GstElement *src = gst_bin_get_by_name_recurse_up(GST_BIN(pipeline), "src");
    bool cleared = false;

    {
        std::lock_guard<std::mutex> lock(appsrc_mutex);
        if (appsrc == src) {
            gst_object_unref(appsrc);
            appsrc = nullptr;
            client_connected = false;
            cleared = true;
        }
    }

    gst_object_unref(src);
    gst_object_unref(pipeline);

    if (cleared) {
        g_print("RTSP client disconnected\n");
    }
}

/* Called when media (pipeline) is created */
static void media_configure(GstRTSPMediaFactory *,
                            GstRTSPMedia *media,
                            gpointer)
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
        std::lock_guard<std::mutex> lock(appsrc_mutex);
        if (appsrc) {
            gst_object_unref(appsrc);
        }
        appsrc = new_appsrc;
    }

    client_connected = true;
    reset_timestamps = true;
    g_print("RTSP client connected\n");

    g_signal_connect(media, "unprepared",
                     (GCallback)media_unprepared, nullptr);

    gst_object_unref(pipeline);
}

/* Push frames from OpenCV */
void push_frames()
{
    const int width = 1280;
    const int height = 720;
    const int fps = 25;
    const GstClockTime frame_duration = gst_util_uint64_scale_int(1, GST_SECOND, fps);
    const auto frame_interval = std::chrono::microseconds(1000000 / fps);

    GstClockTime pts = 0;
    int frame_index = 0;
    auto next_frame_time = std::chrono::steady_clock::now();

    cv::Mat frame(height, width, CV_8UC3);
    cv::Mat yuv(height * 3 / 2, width, CV_8UC1);

    while (true) {
        GstElement *local_appsrc = nullptr;
        {
            std::lock_guard<std::mutex> lock(appsrc_mutex);
            if (appsrc) {
                local_appsrc = GST_ELEMENT(gst_object_ref(appsrc));
            }
        }

        if (!client_connected || !local_appsrc) {
            if (local_appsrc) {
                gst_object_unref(local_appsrc);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (reset_timestamps.exchange(false)) {
            pts = 0;
            frame_index = 0;
            next_frame_time = std::chrono::steady_clock::now();
        }

        frame.setTo(cv::Scalar(0, 0, 0));

        int x = (frame_index * 5) % (width - 200);
        int y = (frame_index * 3) % (height - 120);
        cv::rectangle(frame, cv::Rect(x, y, 200, 120), cv::Scalar(0, 255, 0), -1);

        int x2 = (frame_index * 7) % (width - 160);
        int y2 = (frame_index * 4) % (height - 160);
        cv::rectangle(frame, cv::Rect(x2, y2, 160, 160), cv::Scalar(0, 0, 255), -1);

        int x3 = (frame_index * 11) % (width - 140);
        int y3 = (frame_index * 6) % (height - 100);
        cv::rectangle(frame, cv::Rect(x3, y3, 140, 100), cv::Scalar(255, 0, 0), -1);

        //cv::imshow("preview", frame);
        //cv::waitKey(1);

        cv::cvtColor(frame, yuv, cv::COLOR_BGR2YUV_I420);

        GstBuffer *buffer =
            gst_buffer_new_allocate(nullptr, yuv.total(), nullptr);


        gst_buffer_fill(buffer, 0, yuv.data, yuv.total());

        GST_BUFFER_PTS(buffer) = pts;
        GST_BUFFER_DURATION(buffer) = frame_duration;
        pts += frame_duration;
        frame_index++;

        GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(local_appsrc), buffer);
        if (flow != GST_FLOW_OK) {
            std::lock_guard<std::mutex> lock(appsrc_mutex);
            if (appsrc == local_appsrc) {
                gst_object_unref(appsrc);
                appsrc = nullptr;
                client_connected = false;
            }
        }

        gst_object_unref(local_appsrc);

        // Pace the producer to the target FPS to avoid busy-loop CPU usage.
        next_frame_time += frame_interval;
        auto now = std::chrono::steady_clock::now();
        if (next_frame_time > now) {
            std::this_thread::sleep_until(next_frame_time);
        } else {
            next_frame_time = now;
        }
    }
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);

    /* RTSP server */
    GstRTSPServer *server = gst_rtsp_server_new();
    gst_rtsp_server_set_service(server, "8554");

    GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(server);
    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();

    gst_rtsp_media_factory_set_launch(factory,
        "( appsrc name=src is-live=true format=time "
        "caps=video/x-raw,format=I420,width=1280,height=720,framerate=25/1 "
        "! videoconvert "
        "! x264enc tune=zerolatency speed-preset=ultrafast bitrate=2000 "
        "! rtph264pay name=pay0 pt=96 )");

    gst_rtsp_media_factory_set_shared(factory, FALSE);
    g_signal_connect(factory, "media-configure",
                     (GCallback)media_configure, nullptr);

    gst_rtsp_mount_points_add_factory(mounts, "/main", factory);
    g_object_unref(mounts);

    gst_rtsp_server_attach(server, nullptr);

    g_print("RTSP server running ip:8554/main\n");

    std::thread producer(push_frames);

    GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(loop);

    producer.join();
    return 0;
}
