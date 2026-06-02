# libcamera-streamer

RTP streamer based on libcamera and uvgRTP

## Requirements to build

Install main libs
```
sudo apt install libcamera-dev libspdlog-dev
git clone https://github.com/ultravideo/uvgRTP
```

Install uvgRTP
```
cd uvgRTP
mkdir build && cd build
cmake -DDISABLE_CRYPTO=1 ..
make
sudo make install

Install boost option
```
sudo apt install libboost-program-option-dev
```
Player
vlc.exe rtp.sdp --network-caching=0 --live-caching=0 --file-caching --clock-jitter=0

OK on RPI4 bulleye , libcamera v0.0.5
```
