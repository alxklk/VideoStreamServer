#include "CRTSPServer.h"

#include <functional>

/* Push frames from OpenCV */
void push_frames(CRTSPServer &server)
{
    const int width = CRTSPServer::kWidth;
    const int height = CRTSPServer::kHeight;
    const int fps = CRTSPServer::kFps;
    const auto frame_interval = std::chrono::microseconds(1000000 / fps);

    int frame_index = 0;
    auto next_frame_time = std::chrono::steady_clock::now();

    cv::Mat frame(height, width, CV_8UC3);

    while (true) {
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

        server.Push(frame);
        frame_index++;

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
    CRTSPServer server;
    if (!server.Init(argc, argv)) {
        return 1;
    }

    std::thread producer(push_frames, std::ref(server));
    producer.join();
    return 0;
}
