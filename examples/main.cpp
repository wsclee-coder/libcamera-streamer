#include <string>
#include "spdlog/spdlog.h"

#include "libcamera-streamer/libcamera_streamer.hpp"
#include "libcamera-streamer/streamer_configuration.hpp"

auto main() -> int
{
    //spdlog::set_level(spdlog::level::trace);

    StreamerConfiguration configuration;
    configuration.Camera.width = 640;
    configuration.Camera.height = 480;
    configuration.Camera.framerate = 30;
    configuration.Camera.denoise = "off";
    configuration.Output.Ip = "192.168.0.11";
    configuration.Output.Port = 5004;
    configuration.Encoder.framerate = 30;
    configuration.Encoder.bitrate = 5000000;
    configuration.Encoder.width = 640;
    configuration.Encoder.height = 480;

    const auto streamer = std::make_unique<LibcameraStreamer>(configuration);
    streamer->Start();

    while (true)
    {
    }
    return 0;
}
