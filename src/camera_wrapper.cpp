#include <mutex>
#include <sys/mman.h>

#include "camera_wrapper.hpp"
#include <stdexcept>
#include <spdlog/spdlog.h>

CameraWrapper::CameraWrapper(
    std::unique_ptr<libcamera::CameraManager> cameraManager,
    const std::string &cameraId,
    CameraOptions *options) :
    cameraManager_(std::move(cameraManager))
    , controls_(libcamera::controls::controls)
    , options_(options)
{
    camera_ = cameraManager_->get(cameraId);
    if (!camera_)
    {
        throw std::runtime_error("failed to find camera " + cameraId);
    }

    if (camera_->acquire())
    {
        throw std::runtime_error("failed to acquire camera " + cameraId);
    }

    spdlog::trace("Camera acquired");

    spdlog::trace("START Configuring video");

    const libcamera::StreamRole streamRole = libcamera::StreamRole::VideoRecording;
    configuration_ = camera_->generateConfiguration({streamRole});
    if (!configuration_)
    {
        throw std::runtime_error("failed to generate video configuration");
    }

    libcamera::StreamConfiguration &streamConfiguration = configuration_->at(0);
    streamConfiguration.pixelFormat = libcamera::formats::YUV420;
    // TODO: cfg.bufferCount = ???;
    streamConfiguration.size.width = options_->width;
    streamConfiguration.size.height = options_->height;
    if (streamConfiguration.size.width >= 1280 || streamConfiguration.size.height >= 720)
    {
        streamConfiguration.colorSpace = libcamera::ColorSpace::Rec709;
    }
    else
    {
        streamConfiguration.colorSpace = libcamera::ColorSpace::Smpte170m;
    }

    configuration_->transform = options_->transform;
    controls_.set(libcamera::controls::draft::NoiseReductionMode, libcamera::controls::draft::NoiseReductionModeOff);

    const libcamera::CameraConfiguration::Status validationResult = configuration_->validate();
    if (validationResult == libcamera::CameraConfiguration::Invalid)
    {
        throw std::runtime_error("failed to valid stream configurations");
    }
    if (validationResult == libcamera::CameraConfiguration::Adjusted)
    {
        spdlog::info("Stream configuration adjusted");
    }

    if (camera_->configure(configuration_.get()) < 0)
    {
        throw std::runtime_error("failed to configure streams");
    }

    allocateBuffers();

    spdlog::trace("END Configuring video");
    
    // This makes all the Request objects that we shall need.
    makeRequests();
}

CameraWrapper::~CameraWrapper()
{
}

void CameraWrapper::StartCamera()
{
    // Framerate is a bit weird. If it was set programmatically, we go with
    // that, but otherwise it applies only to preview/video modes. For stills
    // capture we set it as long as possible so that we get whatever the
    // exposure profile wants.
    if (!controls_.get(libcamera::controls::FrameDurationLimits))
    //if (controls_.get(libcamera::controls::FrameDurationLimits).empty())
    //if (controls_.get(libcamera::controls::FrameDurationLimits).size()==0)
    {
        if (options_->framerate > 0)
        {
            int64_t frame_time = 1000000 / options_->framerate; // in us
            controls_.set(
                libcamera::controls::FrameDurationLimits,
                libcamera::Span<const int64_t, 2>({frame_time, frame_time}));
        }
    }

    if (!controls_.get(libcamera::controls::AnalogueGain))
    {
        controls_.set(libcamera::controls::AnalogueGain, options_->gain);
    }
    if (!controls_.get(libcamera::controls::AeMeteringMode))
    {
        controls_.set(libcamera::controls::AeMeteringMode, options_->metering);
    }
    if (!controls_.get(libcamera::controls::AeExposureMode))
    {
        controls_.set(libcamera::controls::AeExposureMode, options_->exposure);
    }
    if (!controls_.get(libcamera::controls::ExposureValue))
    {
        controls_.set(libcamera::controls::ExposureValue, options_->ev);
    }
    if (!controls_.get(libcamera::controls::AwbMode))
    {
        controls_.set(libcamera::controls::AwbMode, options_->awb);
    }

    if (!controls_.get(libcamera::controls::ColourGains) && options_->awb_gain_r
    //if (controls_.get(libcamera::controls::ColourGains).empty() && options_->awb_gain_r
    //if (controls_.get(libcamera::controls::ColourGains).size()==0  && options_->awb_gain_r
        && options_->awb_gain_b)
        controls_.set(libcamera::controls::ColourGains,
                      libcamera::Span<const float, 2>(
                          {options_->awb_gain_r, options_->awb_gain_b}));
    if (!controls_.get(libcamera::controls::Brightness))
        controls_.set(libcamera::controls::Brightness, options_->brightness);
    if (!controls_.get(libcamera::controls::Contrast))
        controls_.set(libcamera::controls::Contrast, options_->contrast);
    if (!controls_.get(libcamera::controls::Saturation))
        controls_.set(libcamera::controls::Saturation, options_->saturation);
    if (!controls_.get(libcamera::controls::Sharpness))
        controls_.set(libcamera::controls::Sharpness, options_->sharpness);

    if (camera_->start(&controls_))
        throw std::runtime_error("failed to start camera");
    controls_.clear();

    camera_->requestCompleted.connect(this, &CameraWrapper::requestComplete);

    for (std::unique_ptr<libcamera::Request> &request : requests_)
    {
        if (camera_->queueRequest(request.get()) < 0)
            throw std::runtime_error("Failed to queue request");
    }
}

void CameraWrapper::makeRequests()
{
    for (libcamera::StreamConfiguration &config : *configuration_)
    {
        spdlog::trace(config.toString());
    }

    auto free_buffers(frame_buffers_);
    while (true)
    {
        for (libcamera::StreamConfiguration &config : *configuration_)
        {
            libcamera::Stream *stream = config.stream();

            if (stream == configuration_->at(0).stream())
            {
                if (free_buffers.empty())
                {
                    spdlog::trace("Requests created");
                    return;
                }
                std::unique_ptr<libcamera::Request> request = camera_->createRequest();
                if (!request)
                {
                    throw std::runtime_error("failed to make request");
                }
                requests_.push_back(std::move(request));
            }
            else if (free_buffers.empty())
            {
                throw std::runtime_error("concurrent streams need matching numbers of buffers");
            }

            libcamera::FrameBuffer *buffer = free_buffers.front();
            free_buffers.pop();
            if (requests_.back()->addBuffer(stream, buffer) < 0)
            {
                throw std::runtime_error("failed to add buffer to request");
            }
        }
    }
}

void CameraWrapper::requestComplete(libcamera::Request *request)
{
    spdlog::trace("CameraWrapper: Request complete");
    if (request->status() == libcamera::Request::RequestCancelled)
    {
        return;
    }
    
        completedRequestsQueue_.enqueue(request);
        requestsToReuseQueue_.enqueue(request);
    
}

libcamera::Request *CameraWrapper::WaitForCompletedRequest()
{
    libcamera::Request *request;
    completedRequestsQueue_.wait_dequeue(request);
    return request;
}

StreamInfo CameraWrapper::GetStreamInfo()
{
    const auto configuration = configuration_->at(0);
    const StreamInfo streamInfo(
        configuration.size.width,
        configuration.size.height,
        configuration.stride,
        configuration.colorSpace.value()
        );

    return streamInfo;
}

libcamera::FrameBuffer *CameraWrapper::GetFrameBufferForRequest(const libcamera::Request *request) const
{
    return request->buffers().at(configuration_->at(0).stream());
}

std::vector<libcamera::Span<uint8_t>> CameraWrapper::Mmap(libcamera::FrameBuffer *buffer) const
{
    const auto item = mapped_buffers_.find(buffer);
    if (item == mapped_buffers_.end())
    {
        return {};
    }
    return item->second;
}

void CameraWrapper::ReuseRequest()
{
    libcamera::Request *request;
    requestsToReuseQueue_.wait_dequeue(request);
    request->reuse(libcamera::Request::ReuseBuffers);
    camera_->queueRequest(request);
}

void CameraWrapper::allocateBuffers()
{
    spdlog::trace("START Frame buffers allocation");

    allocator_ = new libcamera::FrameBufferAllocator(camera_);
    libcamera::Stream *stream = configuration_->at(0).stream();

    if (allocator_->allocate(stream) < 0)
    {
        throw std::runtime_error("failed to allocate capture buffers");
    }

    for (const std::unique_ptr<libcamera::FrameBuffer> &buffer : allocator_->buffers(stream))
    {
        // "Single plane" buffers appear as multi-plane here, but we can spot them because then
        // planes all share the same fd. We accumulate them so as to mmap the buffer only once.
        size_t buffer_size = 0;
        for (unsigned i = 0; i < buffer->planes().size(); i++)
        {
            const auto &plane = buffer->planes()[i];
            buffer_size += plane.length;
            if (i == buffer->planes().size() - 1 || plane.fd.get() != buffer->planes()[i + 1].fd.get())
            {
                void *memory = mmap(nullptr,
                                    buffer_size,
                                    PROT_READ | PROT_WRITE,
                                    MAP_SHARED,
                                    plane.fd.get(),
                                    0);
                mapped_buffers_[buffer.get()].push_back(
                    libcamera::Span<uint8_t>(static_cast<uint8_t *>(memory), buffer_size));
                buffer_size = 0;
            }
        }
        frame_buffers_.push(buffer.get());
    }

    spdlog::trace("END Frame buffers allocation");
}
