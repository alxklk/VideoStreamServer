g++ main.cpp -o main \
    `pkg-config --cflags --libs opencv4` \
    `pkg-config --cflags --libs gstreamer-1.0 gstreamer-rtsp-server-1.0 gstreamer-app-1.0` \
    -pthread