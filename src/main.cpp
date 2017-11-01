#include <librealsense/rs.hpp>
#include <asio.hpp>

#include <cstdio>
#include <stdint.h>
#include <vector>
#include <map>
#include <limits>
#include <iostream>
#include <thread>

#define STB_DXT_IMPLEMENTATION
#include "third_party/stb_dxt.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"

#include "third_party/half.h"

using namespace std::chrono;
using asio::ip::udp;

asio::io_service io_service;
udp::socket s(io_service, udp::endpoint(udp::v4(), 0));
udp::resolver resolver(io_service);
udp::endpoint endpoint;

void streamFrame(const unsigned char * regdepth, const unsigned char * regrgb, uint32_t sequence, rs::intrinsics i_d, float depth_scale);
int openAndStream(std::string serial, std::string pipelineId);
int sendConfig();

bool stream_shutdown = false;

struct DEPTH_DATA_HEADER {
    unsigned char msgType = 0x03;   // 00
    unsigned char deviceID = 0x00;  // 01
    unsigned char unused1 = 0x00;   // 02 => for consistent alignment
    unsigned char unused2 = 0x00;   // 03 => for consistent alignment
    uint32_t sequence = 0;          // 04-07
    unsigned short startRow = 0;    // 08-09
    unsigned short endRow = 0;      // 10-11
};

struct CONFIG_MSG {
    unsigned char msgType = 0x01;    // 00
    unsigned char deviceID = 0x00;   // 01
    unsigned char deviceType = 0x03; // 02
    unsigned char unused1 = 0x00;    // 03 => for consistent alignment
    unsigned short frameWidth = 0;   // 04-05
    unsigned short frameHeight = 0;  // 06-07
    unsigned short maxLines = 0;     // 08-09
    unsigned short unused2 = 0;      // 10-11 => for consistent alignment
    float Cx = 0.0f;                 // 12-15
    float Cy = 0.0f;                 // 16-19
    float Fx = 0.0f;                 // 20-23
    float Fy = 0.0f;                 // 24-27
    float DepthScale = 0.0f;         // 28-31
    char guid[33] = "00000000000000000000000000000000"; // 32-...
};

DEPTH_DATA_HEADER depth_header;
CONFIG_MSG stream_config;

unsigned int headerSize = sizeof(depth_header);
unsigned int linesPerMessage;
unsigned int sendThrottle;
unsigned char * frameStreamBuffer;
int frameStreamBufferSize;
char config_msg_buf[sizeof(stream_config)];

int main(int argc, char *argv[]) {
    std::string portParam("-p");
    std::string ipParam("-d");
    std::string serialParam("-s");
    std::string linesParam("-l");
    std::string sendThrottleParam("-t");
    std::string pipelineParam("-q");
    
    std::string ipValue = "127.0.0.1";
    std::string portValue = "1339";
    std::string serialValue = "";
    std::string linesValue = "-1";
    std::string sendThrottleValue = "1600";
    std::string pipelineValue = "opencl";
    
    if (argc % 2 != 1) {
        std::cout << "Usage:\n  freenectStreamer "
        << portParam << " [port] "
        << ipParam << " [ip] "
        << serialParam << " [serial] "
        << linesParam << " [maxLines] "
        << sendThrottleParam << " [sendThrottle] "
        << pipelineParam << " [pipeline] "
        << "\nDefaults"
        << "\n  port:     " << portValue
        << "\n  ip:       " << ipValue
        << "\n  serial:   " << "\"\" (empty = first available)"
        << "\n  maxLines: " << linesValue << " (-1 = maximum)"
        << "\n  sendThrottle:   " << sendThrottleValue << " (us between line packets; 0 = no limit)"
        << "\n  pipeline: " << pipelineValue << " (one of cpu,opengl,cuda,opencl)"
        << "\n";
        return -1;
    }
    
    for (int count = 1; count < argc; count += 2) {
        if (ipParam.compare(argv[count]) == 0) {
            ipValue = argv[count + 1];
        } else if (portParam.compare(argv[count]) == 0) {
            portValue = argv[count + 1];
        } else if (serialParam.compare(argv[count]) == 0) {
            serialValue = argv[count + 1];
        } else if (linesParam.compare(argv[count]) == 0) {
            linesValue = argv[count + 1];
        } else if (sendThrottleParam.compare(argv[count]) == 0) {
            sendThrottleValue = argv[count + 1];
        } else if (pipelineParam.compare(argv[count]) == 0) {
            pipelineValue = argv[count + 1];
        } else {
            std::cout << "Unknown Parameter: " << argv[count] << std::endl;
        }
    }
    
    sendThrottle = std::stoi(sendThrottleValue);
    int parsedLines = std::stoi(linesValue);
    
    int bytesPerLine = ((640*2)+(640/2));
    int maximumLinesPerPacket = (65506-headerSize)/bytesPerLine;
    
    if (parsedLines > maximumLinesPerPacket) {
        std::cout << "Invalid value for -p. Try > 0 and < 45" << std::endl;
        return -1;
    }
    
    if (parsedLines < 1) {
        linesPerMessage = maximumLinesPerPacket;
    } else {
        linesPerMessage = parsedLines;
    }
    
    frameStreamBufferSize = bytesPerLine * linesPerMessage + headerSize;
    frameStreamBuffer = new unsigned char[frameStreamBufferSize];
    
    endpoint = *resolver.resolve({ udp::v4(), ipValue, portValue });
    
    std::string program_path(argv[0]);
    size_t executable_name_idx = program_path.rfind("cpp-stream");
    
    std::string binpath = "/";
    if (executable_name_idx != std::string::npos) {
        binpath = program_path.substr(0, executable_name_idx);
    }
    std::cout << "Running in: " << binpath << std::endl;
    
    return openAndStream(serialValue, pipelineValue);
}

int openAndStream(std::string serial, std::string pipelineId) try {
    rs::log_to_console(rs::log_severity::warn);
    //rs::log_to_file(rs::log_severity::debug, "librealsense.log");

    rs::context ctx;
    printf("There are %d connected RealSense devices.\n", ctx.get_device_count());
    if(ctx.get_device_count() == 0) return EXIT_FAILURE;

    if (serial == "") {
        serial = ctx.get_device(0)->get_serial();
    }
    
    rs::device * dev = NULL;
    
    int device = 0;
    while (device < ctx.get_device_count()) {
        if (serial.compare(ctx.get_device(device)->get_serial()) == 0) {
            dev = ctx.get_device(device);
            break;
        }
        device++;
    }
    
    if (dev == NULL) {
        printf("\nFailed to find device with serial %s\n", serial.c_str());
        return -1;
    }
    
    printf("\nUsing device 0, an %s\n", dev->get_name());
    printf("    Serial number: %s\n", dev->get_serial());
    printf("    Firmware version: %s\n\n", dev->get_firmware_version());

    dev->enable_stream(rs::stream::depth, rs::preset::best_quality);
    dev->enable_stream(rs::stream::color, 0, 0, rs::format::bgra8, 0);
    
    rs::apply_ivcam_preset(dev, RS_IVCAM_PRESET_LONG_RANGE); //RS_IVCAM_PRESET_LONG_RANGE
    /* activate video streaming */
    dev->start();
    
    unsigned int fpsCounter = 0;
    milliseconds lastFpsAverage = duration_cast< milliseconds >(system_clock::now().time_since_epoch());
    milliseconds lastFrameSent = duration_cast< milliseconds >(system_clock::now().time_since_epoch());
    milliseconds interval = milliseconds(2000);
    
    asio::socket_base::send_buffer_size sbsoption(frameStreamBufferSize*linesPerMessage*2);
    s.set_option(sbsoption);

    asio::socket_base::send_low_watermark slwoption(frameStreamBufferSize);
    s.set_option(slwoption);
    
    dev->wait_for_frames();
    
    int w_d = dev->get_stream_width(rs::stream::depth);
    int h_d = dev->get_stream_height(rs::stream::depth);
    rs::format f_d = dev->get_stream_format(rs::stream::depth);
    rs::intrinsics i_d = dev->get_stream_intrinsics(rs::stream::depth);
    
    int w_c = dev->get_stream_width(rs::stream::color_aligned_to_depth);
    int h_c = dev->get_stream_height(rs::stream::color_aligned_to_depth);
    float depthScale = dev->get_depth_scale();
    rs::format f_c = dev->get_stream_format(rs::stream::color_aligned_to_depth);
    
    std::cout << "\nDepth:\n";
    std::cout << "\tScale: " << depthScale << "\n";
    std::cout << "\tW: " << w_d << " H: " << h_d << "\n";
    std::cout << "\tFormat: " << f_d << "\n";
    std::cout << "\tIntrinsics:\n\t\tvfov: " << i_d.vfov() << " hfov: " << i_d.hfov() << "\n";
    std::cout << "\t\twidth: " << i_d.width << " height:" << i_d.height << "\n";
    std::cout << "\t\tfx: " << i_d.fx << " fy: " << i_d.fy << "\n";
    std::cout << "\t\tppx: " << i_d.ppx << " ppy: " << i_d.ppy << "\n";
    
    std::cout << "\nColor:\n";
    std::cout << "\tW: " << w_c << " H: " << h_c << "\n";
    std::cout << "\tFormat: " << f_c << "\n";
    
    stream_config.frameWidth = (unsigned short) w_d;
    stream_config.frameHeight = (unsigned short) h_d;
    stream_config.maxLines = (unsigned short) linesPerMessage;
    stream_config.Cx = i_d.ppx;
    stream_config.Cy = i_d.ppy;
    stream_config.Fx = i_d.fx;
    stream_config.Fy = i_d.fy;
    stream_config.DepthScale = depthScale;
    std::strcpy(stream_config.guid, dev->get_serial());
    
    while (!stream_shutdown) {
        
        milliseconds ms = duration_cast< milliseconds >(system_clock::now().time_since_epoch());
        
        const unsigned char * depth_data = (const unsigned char *) dev->get_frame_data(rs::stream::depth);
        const unsigned char * color_data = (const unsigned char *) dev->get_frame_data(rs::stream::color_aligned_to_depth);
        rs::intrinsics i_d = dev->get_stream_intrinsics(rs::stream::depth);
        uint32_t sequence = dev->get_frame_number(rs::stream::depth);
        
        std::thread streamFrameThread(streamFrame, depth_data, color_data, sequence, i_d, dev->get_depth_scale());
        
        dev->wait_for_frames();
        streamFrameThread.join();
        
        fpsCounter++;
        lastFrameSent = ms;
        ms = duration_cast< milliseconds >(system_clock::now().time_since_epoch());
        if (lastFpsAverage + interval <= ms) {
            lastFpsAverage = ms;
            std::cout << "Average FPS: " << fpsCounter / 2.0 << std::endl;
            fpsCounter = 0;
            sendConfig();
        }
    }
    
    return EXIT_SUCCESS;
}
catch(const rs::error & e)
{
    std::cerr << "RealSense error calling " << e.get_failed_function() << "(" << e.get_failed_args() << "):\n    " << e.what() << std::endl;
    return EXIT_FAILURE;
}
catch(const std::exception & e)
{
    std::cerr << e.what() << std::endl;
    return EXIT_FAILURE;
}
    
int sendConfig() {
    memcpy(config_msg_buf, &stream_config, sizeof(stream_config));
    
    try {
        s.send_to(asio::buffer(config_msg_buf, sizeof(stream_config)), endpoint);
    } catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return -1;
    }
    
    return 0;
}
    
void streamFrame(const unsigned char * regdepth, const unsigned char * regrgb, uint32_t sequence, rs::intrinsics i_d, float depth_scale) {
    depth_header.sequence = sequence;
    
    for (unsigned int startRow = 0; startRow < i_d.height; startRow += linesPerMessage) {
        
        size_t endRow = startRow + linesPerMessage;
        if (endRow >= i_d.height) endRow = i_d.height;
        if (startRow >= endRow) break;
        
        size_t totalLines = endRow - startRow;
        
        depth_header.startRow = (unsigned short) startRow;
        depth_header.endRow = (unsigned short) endRow;
        
        memcpy(&frameStreamBuffer[0], &depth_header, headerSize);
        size_t writeOffset = headerSize;
        
        //std::cout << writeOffset << " == " << sizeof(depth_header) << "\n";
        size_t depthLineSizeR = i_d.width * 2;
        size_t readOffset = startRow*depthLineSizeR;
        
        // uint16_t hf = half_from_float(shortFromRegDepth * depth_scale);
        //  ... then use https://docs.unity3d.com/ScriptReference/TextureFormat.RHalf.html
        // or, divide into two chars that, when multiplied, result into the original short (tricky when prime :P )
        //  ... then use https://docs.unity3d.com/ScriptReference/TextureFormat.RG16.html
        
        
        memcpy(&frameStreamBuffer[writeOffset], &regdepth[readOffset], totalLines*depthLineSizeR);
        writeOffset += totalLines*depthLineSizeR;
        
        size_t colorLineSizeR = i_d.width * 4; // BGRA8
        readOffset = startRow*colorLineSizeR;
        stb_compress_dxt(&frameStreamBuffer[writeOffset], &regrgb[readOffset], (int)i_d.width, (int)totalLines, 0);
        
        try {
            
            int res = s.send_to(asio::buffer(frameStreamBuffer, frameStreamBufferSize), endpoint);
            if (res < frameStreamBufferSize) {
                std::cout << "Faild to send\n";
            }
        } catch (std::exception& e) {
            std::cerr << "Exception: " << e.what() << "\n";
        }
        
        usleep(sendThrottle);
    }
}

