/**
track_targets
https://github.com/fnoop/vision_landing

** Intel Realsense Branch **

This program uses opencv and aruco to detect fiducial markers in a stream.
It uses tracking across images in order to avoid ambiguity problem with a single marker.
For each target detected, it performs pose estimation and outputs the marker ID and translation vectors.
These vectors are used by vision_landing mavlink messages to enable precision landing by vision alone.

Compile with cmake: cmake . && make
 or manually: g++ src/track_targets.cpp -o track_targets -std=gnu++11 `pkg-config --cflags --libs aruco`

Run separately with: ./track_targets -d TAG36h11 /dev/video0 calibration.yml 0.235
./track_targets -w 1280 -g 720 -d TAG36h11 -o 'appsrc ! autovideoconvert ! v4l2video11h264enc extra-controls="encode,h264_level=10,h264_profile=4,frame_level_rate_control_enable=1,video_bitrate=2097152" ! h264parse ! rtph264pay config-interval=1 pt=96 ! udpsink host=192.168.1.70 port=5000 sync=false' /dev/video2 calibration/ocam5cr-calibration-1280x720.yml 0.235
./track_targets -w 1280 -g 720 -d TAG36h11 -o /srv/maverick/data/videos/landing.avi /dev/video2 calibration/ocam5cr-calibration-1280x720.yml 0.235
./track_targets -b 0.8 -d TAG36h11 -o 'appsrc ! autovideoconvert ! v4l2video11h264enc extra-contros="encode,h264_level=10,h264_profile=4,frame_level_rate_control_enable=1,video_bitrate=2097152" ! h264parse ! rtph264pay config-interval=1 pt=96 ! udpsink host=192.168.1.70 port=5000 sync=false' -i 1 -v /dev/video2 calibration/ocam5cr-calibration-640x480.yml 0.235
**/

#include <iostream>
#include <sstream>
#include <string>
#include <queue>
#include <signal.h>
#include <poll.h>
#include <ctime>
#include <sys/select.h>
#include "args.hxx"
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/calib3d/calib3d.hpp>
#include "aruco/aruco.h"
#include <librealsense/rs.hpp>

using namespace cv;
using namespace aruco;

// Setup sig handling
static volatile sig_atomic_t sigflag = 0;
static volatile sig_atomic_t stateflag = 0; // 0 = stopped, 1 = started
void handle_sig(int sig) {
    cout << "SIGNAL:" << sig << ":Received" << endl;
    sigflag = 1;
}
void handle_sigusr1(int sig) {
    cout << "SIGNAL:SIGUSR1:Received:" << sig << ":Switching on Vision Processing" << endl;
    stateflag = 1;
}
void handle_sigusr2(int sig) {
    cout << "SIGNAL:SIGUSR2:Received:" << sig << ":Switching off Vision Processing" << endl;
    stateflag = 0;
}

// Setup fps tracker
double CLOCK()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,  &t);
    return (t.tv_sec * 1000)+(t.tv_nsec*1e-6);
}
double _avgdur=0;
double _fpsstart=0;
double _avgfps=0;
double _fps1sec=0;
double avgdur(double newdur)
{
    _avgdur=0.98*_avgdur+0.02*newdur;
    return _avgdur;
}
double avgfps()
{
    if(CLOCK()-_fpsstart>1000)      
    {
        _fpsstart=CLOCK();
        _avgfps=0.7*_avgfps+0.3*_fps1sec;
        _fps1sec=0;
    }
    _fps1sec++;
    return _avgfps;
}

// Define function to draw AR landing marker
void drawARLandingCube(cv::Mat &Image, Marker &m, const CameraParameters &CP, Scalar color) {
    Mat objectPoints(8, 3, CV_32FC1);
    double halfSize = m.ssize / 2;

    objectPoints.at< float >(0, 0) = -halfSize;
    objectPoints.at< float >(0, 1) = -halfSize;
    objectPoints.at< float >(0, 2) = 0;
    objectPoints.at< float >(1, 0) = halfSize;
    objectPoints.at< float >(1, 1) = -halfSize;
    objectPoints.at< float >(1, 2) = 0;
    objectPoints.at< float >(2, 0) = halfSize;
    objectPoints.at< float >(2, 1) = halfSize;
    objectPoints.at< float >(2, 2) = 0;
    objectPoints.at< float >(3, 0) = -halfSize;
    objectPoints.at< float >(3, 1) = halfSize;
    objectPoints.at< float >(3, 2) = 0;

    objectPoints.at< float >(4, 0) = -halfSize;
    objectPoints.at< float >(4, 1) = -halfSize;
    objectPoints.at< float >(4, 2) = m.ssize;
    objectPoints.at< float >(5, 0) = halfSize;
    objectPoints.at< float >(5, 1) = -halfSize;
    objectPoints.at< float >(5, 2) = m.ssize;
    objectPoints.at< float >(6, 0) = halfSize;
    objectPoints.at< float >(6, 1) = halfSize;
    objectPoints.at< float >(6, 2) = m.ssize;
    objectPoints.at< float >(7, 0) = -halfSize;
    objectPoints.at< float >(7, 1) = halfSize;
    objectPoints.at< float >(7, 2) = m.ssize;

    vector< Point2f > imagePoints;
    cv::projectPoints(objectPoints, m.Rvec, m.Tvec, CP.CameraMatrix, CP.Distorsion, imagePoints);
    // draw lines of different colours
    for (int i = 0; i < 4; i++)
        cv::line(Image, imagePoints[i], imagePoints[(i + 1) % 4], color, 1, CV_AA);

    for (int i = 0; i < 4; i++)
        cv::line(Image, imagePoints[i + 4], imagePoints[4 + (i + 1) % 4], color, 1, CV_AA);

    for (int i = 0; i < 4; i++)
        cv::line(Image, imagePoints[i], imagePoints[i + 4], color, 1, CV_AA);
}

// Print the calculated distance at bottom of image
void drawVectors(Mat &in, Scalar color, int lineWidth, int vOffset, int MarkerId, double X, double Y, double Distance, double cX, double cY) {
    char cad[100];
    sprintf(cad, "ID:%i, Distance:%0.3fm, X:%0.3f, Y:%0.3f, cX:%0.3f, cY:%0.3f", MarkerId, Distance, X, Y, cX, cY);
    Point cent(10, vOffset);
    cv::putText(in, cad, cent, FONT_HERSHEY_SIMPLEX, std::max(0.5f,float(lineWidth)*0.3f), color, lineWidth);
}

// Summarise a marker history over the past 10 frames
int markerHistory(map<uint32_t, queue<int>> &marker_history_queue, uint32_t thisId, uint32_t marker_history) {
    // Work out current history for this marker
    uint32_t histsum = 0;
    for (unsigned int j = 0; j < marker_history; j++) {
        uint32_t _val = marker_history_queue[thisId].front(); // fetch value off front of queue
        histsum += _val; // add value to history sum
        marker_history_queue[thisId].pop(); // pop value off front of queue
        marker_history_queue[thisId].push(_val); // push value back onto end of queue
    }
    return histsum;
}

// Change active marker and reset all marker histories
void changeActiveMarker(map<uint32_t, queue<int>> &marker_history_queue, uint32_t &active_marker, uint32_t newId, uint32_t marker_history) {
    // Set the new marker as active
    active_marker = newId;

    // Reset all marker histories.  This ensures that another marker change can't happen for at least x frames where x is history size
    for (auto & markerhist:marker_history_queue) {
        for (unsigned int j = 0; j < marker_history; j++) {
            marker_history_queue[markerhist.first].push(0); marker_history_queue[markerhist.first].pop();
        }
    }
}

// main..
int main(int argc, char** argv) {
    // Unbuffer stdout and stdin
    cout.setf(ios::unitbuf);
    ios_base::sync_with_stdio(false);

    // Setup arguments for parser
    args::ArgumentParser parser("Track fiducial markers and estimate pose, output translation vectors for vision_landing");
    args::HelpFlag help(parser, "help", "Display this help menu", {'h', "help"});
    args::Flag verbose(parser, "verbose", "Verbose", {'v', "verbose"});
    args::ValueFlag<int> markerid(parser, "markerid", "Marker ID", {'i', "id"});
    args::ValueFlag<string> dict(parser, "dict", "Marker Dictionary", {'d', "dict"});
    args::ValueFlag<string> output(parser, "output", "Output Stream", {'o', "output"});
    args::ValueFlag<int> width(parser, "width", "Video Input Resolution - Width", {'w', "width"});
    args::ValueFlag<int> height(parser, "height", "Video Input Resolution - Height", {'g', "height"});
    args::ValueFlag<int> fps(parser, "fps", "Video Output FPS - Kludge factor", {'f', "fps"});
    args::ValueFlag<double> brightness(parser, "brightness", "Camera Brightness/Gain", {'b', "brightness"});
    args::ValueFlag<string> sizemapping(parser, "sizemapping", "Marker Size Mappings, in marker_id:size format, comma separated", {'z', "sizemapping"});
    args::ValueFlag<int> markerhistory(parser, "markerhistory", "Marker tracking history, in frames", {"markerhistory"});
    args::ValueFlag<int> markerthreshold(parser, "markerthreshold", "Marker tracking threshold, percentage", {"markerthreshold"});
    args::ValueFlag<string> fourcc(parser, "fourcc", "FourCC CoDec code", {'c', "fourcc"});
    args::ValueFlag<string> streamtype(parser, "streamtype", "Realsense Stream Type", {'s', "streamtype"});
    args::ValueFlag<string> camprofile(parser, "camprofile", "Realsense Camera Profile", {'p', "camprofile"});
    args::ValueFlag<int> colormap(parser, "colormap", "Colormap IR/Depth data", {"colormap"});
    args::Positional<string> input(parser, "input", "Input Stream");
    args::Positional<string> calibration(parser, "calibration", "Calibration Data");
    args::Positional<double> markersize(parser, "markersize", "Marker Size");
    
    // Parse arguments
    try
    {
        parser.ParseCLI(argc, argv);
    }
    catch (args::Help)
    {
        std::cout << parser;
        return 0;
    }
    catch (args::ParseError e)
    {
        std::cerr << e.what() << std::endl;
        std::cerr << parser;
        return 1;
    }
    catch (args::ValidationError e)
    {
        std::cerr << e.what() << std::endl;
        std::cerr << parser;
        return 1;
    }
    
    if (!input || !calibration || !markersize) {
        std::cout << parser;
        return 1;
    }
    
    // Register signals
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);
    signal(SIGUSR1, handle_sigusr1);
    signal(SIGUSR2, handle_sigusr2);

    // If resolution is specified then use, otherwise use default
    int inputwidth = 640;
    int inputheight = 480;
    if (width)
        inputwidth = args::get(width);
    if (height)
        inputheight = args::get(height);
        
    // If fps is specified then use, otherwise use default
    // Note this doesn't seem to matter for network streaming, only when writing to file
    int inputfps = 30;
    if (fps)
        inputfps = args::get(fps);
    
    // If realsense streamtype is specified then use, otherwise set to colour stream
    string inputstream = "colour";
    if (streamtype)
        inputstream = args::get(streamtype);

    // If brightness is specified then use, otherwise use default
    double inputbrightness = 0.5;
    if (brightness)
        inputbrightness = args::get(brightness);

    string inputprofile = "outdoors";
    if (camprofile)
        inputprofile = args::get(camprofile);
    
    int inputcolormap = 2;
    if (colormap)
        inputcolormap = args::get(colormap);
    
    /* Instead of opening a VideoCapture object, use librealsense */
    rs::context ctx;
    if(ctx.get_device_count() == 0) throw std::runtime_error("No RealSense camera detected");
    rs::device * dev = ctx.get_device(0);
    cout << "info:realsense:device:" << dev->get_name() << endl;
    cout << "info:realsense:serialno:" << dev->get_serial() << endl;
    cout << "info:realsense:firmware:" << dev->get_firmware_version() << endl;
    
    // Pick input stream depending on streamtype setting
    if (inputstream == "colour") {
        cout << "info:realsense:stream:colour" << endl;
        dev->enable_stream(rs::stream::color, inputwidth, inputheight, rs::format::bgr8, inputfps);
    } else if (inputstream == "infrared" || inputstream == "hybrid") {
        cout << "info:realsense:stream:" << inputstream << endl;
        dev->enable_stream(rs::stream::color, inputwidth, inputheight, rs::format::bgr8, inputfps);
        // Configure Infrared stream
        dev->enable_stream(rs::stream::infrared, inputwidth, inputheight, rs::format::y8, inputfps);
        // Must also configure depth stream for IR stream to run properly
        dev->enable_stream(rs::stream::depth, inputwidth, inputheight, rs::format::z16, inputfps);
    }
    dev->start();

    if (inputprofile == "outdoors") {
        cout << "info::realsense:camprofile:outdoors" << endl;
        // Set camera settings - https://github.com/IntelRealSense/librealsense/issues/208#issuecomment-234763197 
        dev->set_option(rs::option::r200_emitter_enabled, 0); //disable IR laser pattern
        // dev->set_option(rs::option::color_enable_auto_exposure, 1); // disable/manual exposure
        dev->set_option(rs::option::color_enable_auto_exposure, 3); // aperture priority / automatic exposure
        dev->set_option(rs::option::color_backlight_compensation, 0);
        dev->set_option(rs::option::color_brightness, 62);
        dev->set_option(rs::option::color_contrast, 38);
        dev->set_option(rs::option::color_contrast, 20);
        dev->set_option(rs::option::color_enable_auto_white_balance, 0);
        dev->set_option(rs::option::color_gain, 88);
        dev->set_option(rs::option::color_gamma, 180);
        dev->set_option(rs::option::color_hue, -1018);
        // color_optical_frame_id: camera_rgb_optical_frame
        dev->set_option(rs::option::color_saturation, 87);
        dev->set_option(rs::option::color_sharpness, 7);
        dev->set_option(rs::option::color_white_balance, 2987);
        /*
        dev->set_option(rs::option::r200_auto_exposure_bottom_edge: 239);
        dev->set_option(rs::option::r200_auto_exposure_left_edge: 0);
        dev->set_option(rs::option::r200_auto_exposure_right_edge: 319);
        dev->set_option(rs::option::r200_auto_exposure_top_edge: 0);
        */
        dev->set_option(rs::option::r200_lr_auto_exposure_enabled, 1);
        dev->set_option(rs::option::r200_lr_exposure, 8);
        dev->set_option(rs::option::r200_lr_gain, 100);
    }
    
    // Camera warmup - Dropped several first frames to let auto-exposure stabilize and settings to take effect
    for(int i = 0; i < 30; i++) dev->wait_for_frames();

    // Read and parse camera calibration data
    aruco::CameraParameters CamParam;
    CamParam.readFromXMLFile(args::get(calibration));
    if (!CamParam.isValid()) {
        cerr << "Calibration Parameters not valid" << endl;
        return -1;
    }

    // Take a single image and resize calibration parameters based on input stream dimensions
    Mat rawimage(Size(inputwidth, inputheight), inputstream == "colour" ? CV_8UC3 : CV_8UC1, inputstream == "colour" ? (void*)dev->get_frame_data(rs::stream::color) :  (void*)dev->get_frame_data(rs::stream::infrared), Mat::AUTO_STEP);
    /*
    // Probably unnecessary to convert gray to rgb, aruco only converts it staright back to gray again.
    if (inputstream == "infrared")
        cvtColor(rawimage,rawimage,CV_GRAY2RGB);
    */
    CamParam.resize(rawimage.size());

    // Calculate the fov from the calibration intrinsics
    const double pi = std::atan(1)*4;
    const double fovx = 2 * atan(inputwidth / (2 * CamParam.CameraMatrix.at<float>(0,0))) * (180.0/pi); 
    const double fovy = 2 * atan(inputheight / (2 * CamParam.CameraMatrix.at<float>(1,1))) * (180.0/pi);
    cout << "info:FoVx~" << fovx << ":FoVy~" << fovy << ":vWidth~" << inputwidth << ":vHeight~" << inputheight << endl;

    // Create an output object, if output specified then setup the pipeline unless output is set to 'window'
    VideoWriter writer;
    if (output && args::get(output) != "window") {
        if (fourcc) {
            string _fourcc = args::get(fourcc);
            writer.open(args::get(output), CV_FOURCC(_fourcc[0], _fourcc[1], _fourcc[2], _fourcc[3]), inputfps, cv::Size(inputwidth, inputheight), true);
        } else {
            writer.open(args::get(output), 0, inputfps, cv::Size(inputwidth, inputheight), true);
        }
        if (!writer.isOpened()) {
            cerr << "Error can't create video writer" << endl;
            return 1;
        }
    }

    // Setup the marker detection
    double MarkerSize = args::get(markersize);
    MarkerDetector MDetector;
    MDetector.setThresholdParams(7, 7);
    MDetector.setThresholdParamRange(2, 0);
    std::map<uint32_t,MarkerPoseTracker> MTracker; // use a map so that for each id, we use a different pose tracker
    if (dict)
        MDetector.setDictionary(args::get(dict), 0.f);

    // Start framecounter at 0 for fps tracking
    int frameno=0;

    // Create a map of marker sizes from 'sizemapping' config setting
    map<uint32_t,float> markerSizes;
    stringstream ss(args::get(sizemapping));
    // First split each mapping into a vector
    vector<string> _size_mappings;
    string _size_mapping;
    while(std::getline(ss, _size_mapping, ',')) {
        _size_mappings.push_back( _size_mapping );
    }
    // Next tokenize each vector element into the markerSizes map
    for (std::string const & _s : _size_mappings) {
        auto _i = _s.find(':');
        markerSizes[atoi(_s.substr(0,_i).data())] = atof(_s.substr(_i+1).data());
    }
    // Debug print the constructed map
    cout << "info:Size Mappings:";
    for (const auto &p : markerSizes) {
        cout << p.first << "=" << p.second << ", ";
    }
    cout << endl;

    // Setup marker thresholding
    uint32_t active_marker;
    // Use a map of queues to track state of each marker in the last x frames
    map<uint32_t, queue<int>> marker_history_queue;
    
    // If marker history or threshold is set in parameters use them, otherwise set defaults
    uint32_t marker_history;
    if (args::get(markerhistory)) {
        marker_history = args::get(markerhistory);
    } else {
        marker_history = 15;
    }
    cout << "debug:Marker History:" << marker_history << endl;
    uint32_t marker_threshold;
    if (args::get(markerthreshold)) {
        marker_threshold = args::get(markerthreshold);
    } else {
        marker_threshold = 50;
    }
    cout << "debug:Marker Threshold:" << marker_threshold << endl;
    
    // Main loop
    while (true) {

        // If signal for interrupt/termination was received, break out of main loop and exit
        if (sigflag) {
            cout << "info:Signal Detected:Exiting track_targets" << endl;
            break;
        }

        // If tracking not active, skip
        if (!stateflag) {
            // Add a 1ms sleep to slow down the loop if nothing else is being done
            nanosleep((const struct timespec[]){{0, 10000000L}}, NULL);
            continue;
        }

        // Lodge clock for start of frame
        double framestart=CLOCK();
        
        // Copy image from input stream to cv matrix, skip iteration if empty
        dev->wait_for_frames();
        Mat rawimage(Size(inputwidth, inputheight), inputstream == "colour" ? CV_8UC3 : CV_8UC1, inputstream == "colour" ? (void*)dev->get_frame_data(rs::stream::color) :  (void*)dev->get_frame_data(rs::stream::infrared), Mat::AUTO_STEP);
        if (rawimage.empty()) continue;

        // Detect markers
        vector< Marker > Markers=MDetector.detect(rawimage);

        // Order the markers in ascending size - we want to start with the smallest.
        map<float, uint32_t> markerAreas; 
        map<uint32_t, bool> markerIds;
        for (auto & marker:Markers) {
            markerAreas[marker.getArea()] = marker.id;
            markerIds[marker.id] = true;
            // If the marker doesn't already exist in the threshold tracking, add and populate with full set of zeros
            if (marker_history_queue.count(marker.id) == 0) {
                for (unsigned int i=0; i<marker_history; i++) marker_history_queue[marker.id].push(0);
            }
        }

        // Iterate through marker history and update for this frame
        for (auto & markerhist:marker_history_queue) {
            // If marker was detected in this frame, push a 1
            (markerIds.count(markerhist.first)) ? markerhist.second.push(1) : markerhist.second.push(0);
            // If the marker history has reached history limit, pop the oldest element
            if (markerhist.second.size() > marker_history) {
                markerhist.second.pop();
            }
            
        }
        
        // If marker is set in config, use that to lock on
        if (markerid) {
            active_marker = args::get(markerid);
        // Otherwise find the smallest marker that has a size mapping
        } else {
            for (auto & markerArea:markerAreas) {
                uint32_t thisId = markerArea.second;
                if (markerSizes[thisId]) {
                    // If the current history for this marker is >threshold, then set as the active marker and clear marker histories.  Otherwise, skip to the next sized marker.
                    uint32_t _histsum = markerHistory(marker_history_queue, thisId, marker_history);
                    float _histthresh = marker_history * ((float)marker_threshold / (float)100);
                    if (_histsum > _histthresh) {
                        if (active_marker == thisId) break; // Don't change to the same thing
                        cout << "debug:changing active_marker:" << thisId << ":" << _histsum << ":" << _histthresh << ":" << endl;
                        changeActiveMarker(marker_history_queue, active_marker, thisId, marker_history);
                        if (verbose) {
                            cout << "debug:marker history:";
                            for (auto & markerhist:marker_history_queue) {
                                cout << markerhist.first << ":" << markerHistory(marker_history_queue, markerhist.first, marker_history) << ":";
                            }
                            cout << endl;
                        }
                        break;
                    }
                }
            }
        }
        // If a marker lock hasn't been found by this point, use the smallest found marker with the default marker size
        if (!active_marker) {
            for (auto & markerArea:markerAreas) {
                uint32_t thisId = markerArea.second;
                // If the history threshold for this marker is >50%, then set as the active marker and clear marker histories.  Otherwise, skip to the next sized marker.
                uint32_t _histsum = markerHistory(marker_history_queue, thisId, marker_history);
                float _histthresh = marker_history * ((float)marker_threshold / (float)100);
                if (_histsum > _histthresh) {
                    cout << "debug:changing active_marker:" << thisId << endl;
                    changeActiveMarker(marker_history_queue, active_marker, thisId, marker_history); 
                    break;
                }
            }
        }

        // Iterate through the markers, in order of size, and do pose estimation
        for (auto & markerArea:markerAreas) {
            if (markerArea.second != active_marker) continue; // Don't do pose estimation if not active marker, save cpu cycles
            float _size;
            // If marker size mapping exists for this marker, use it for pose estimation
            if (markerSizes[markerArea.second]) {
                _size = markerSizes[markerArea.second];
            // Otherwise use generic marker size
            } else if (MarkerSize) {
                _size = MarkerSize;
            }
            // Find the Marker in the Markers map and do pose estimation.  I'm sure there's a better way of iterating through the map..
            for (unsigned int i = 0; i < Markers.size(); i++) {
                if (Markers[i].id == markerArea.second)  {
                    MTracker[markerArea.second].estimatePose(Markers[i],CamParam,_size);
                }
            }
        }
    
        // Temp - if infrared stream, also retrieve depth image and use that to overlay AR markers
        // http://www.samontab.com/web/2016/04/interfacing-intel-realsense-f200-with-opencv/
        // https://software.intel.com/en-us/articles/using-librealsense-and-opencv-to-stream-rgb-and-depth-data
        if (inputstream == "hybrid") {
           // Create depth image
           cv::Mat depth16( inputheight,
              inputwidth,
              CV_16U,
              (uchar *)dev->get_frame_data(rs::stream::depth));
            cv::Mat depth8u = depth16;
            depth8u.convertTo( depth8u, CV_8UC1, 255.0/1000 );
            // depth8u.convertTo( depth8u, CV_8UC1, 1/256.0 );
           // cv::Mat depth8u;
           // depth8u.convertTo( depth8u, CV_8UC1, 255.0/1000 );
           // depth16.convertTo( depth8u, CV_8UC1 );
            // equalizeHist(depth8u, depth8u);
            if (inputcolormap >= 0) {
                applyColorMap(depth8u, rawimage, inputcolormap);
            } else {
                applyColorMap(depth8u, rawimage, COLORMAP_COOL);
            }
           // depth16.convertTo( rawimage, CV_8UC3 );
           // cvtColor(depth8u,rawimage,CV_GRAY2RGB);
           // rawimage = depth8u;
        } else if (inputstream == "infrared") {
            if (inputcolormap >= 0) {
                equalizeHist(rawimage, rawimage);
                applyColorMap(rawimage, rawimage, inputcolormap);
            } else {
                cvtColor(rawimage,rawimage,CV_GRAY2RGB);
            }
        }

        // Iterate through each detected marker and send data for active marker and draw green AR cube, otherwise draw red AR square
        for (unsigned int i = 0; i < Markers.size(); i++) {
            // If marker id matches current active marker, draw a green AR cube
            if (Markers[i].id == active_marker) {
                if (output) {
                    if (inputcolormap >= 0) {
                        Markers[i].draw(rawimage, Scalar(255, 255, 255), 2, false);
                    } else {
                        Markers[i].draw(rawimage, Scalar(0, 255, 0), 2, false);
                    }
                }
                // If pose estimation was successful, draw AR cube and distance
                if (Markers[i].Tvec.at<float>(0,2) > 0) {
                    // Calculate angular offsets in radians of center of detected marker
                    double xoffset = (Markers[i].getCenter().x - inputwidth / 2.0) * (fovx * (pi/180)) / inputwidth;
                    double yoffset = (Markers[i].getCenter().y - inputheight / 2.0) * (fovy * (pi/180)) / inputheight;
                    if (verbose)
                        cout << "debug:active_marker:" << active_marker << ":center~" << Markers[i].getCenter() << ":area~" << Markers[i].getArea() << ":marker~" << Markers[i] << endl;
                    cout << "target:" << Markers[i].id << ":" << xoffset << ":" << yoffset << ":" << Markers[i].Tvec.at<float>(0,2) << endl;
                    if (output) { // don't burn cpu cycles if no output
                        if (inputcolormap >= 0) {
                            drawARLandingCube(rawimage, Markers[i], CamParam, Scalar(255, 255, 255, 255));
                        } else {
                            drawARLandingCube(rawimage, Markers[i], CamParam, Scalar(0, 255, 0, 255));
                        }
                        CvDrawingUtils::draw3dAxis(rawimage, Markers[i], CamParam);
                        if (inputcolormap >= 0) {
                            drawVectors(rawimage, Scalar (255,255,255), 1, (i+1)*20, Markers[i].id, xoffset, yoffset, Markers[i].Tvec.at<float>(0,2), Markers[i].getCenter().x, Markers[i].getCenter().y);
                        } else {
                            drawVectors(rawimage, Scalar (0,255,0), 1, (i+1)*20, Markers[i].id, xoffset, yoffset, Markers[i].Tvec.at<float>(0,2), Markers[i].getCenter().x, Markers[i].getCenter().y);
                        }
                    }
                }
            // Otherwise draw a red square
            } else {
                if (output) { // don't burn cpu cycles if no output
                    if (inputcolormap >= 0) {
                        Markers[i].draw(rawimage, Scalar(0, 0, 0), 2, false);
                        drawVectors(rawimage, Scalar (0,0,0), 1, (i+1)*20, Markers[i].id, 0, 0, Markers[i].Tvec.at<float>(0,2), Markers[i].getCenter().x, Markers[i].getCenter().y);
                    } else {
                        Markers[i].draw(rawimage, Scalar(0, 0, 255), 2, false);
                        drawVectors(rawimage, Scalar (0,0,255), 1, (i+1)*20, Markers[i].id, 0, 0, Markers[i].Tvec.at<float>(0,2), Markers[i].getCenter().x, Markers[i].getCenter().y);
                    }
                }
            }
        }

        if (output && args::get(output) != "window") {
            writer << rawimage;
        } else if (output && args::get(output) == "window") {
            imshow("vision_landing", rawimage);
        }
    
        // Lodge clock for end of frame    
        double framedur = CLOCK()-framestart;

        // Print fps info every 100 frames if in debug mode
        char framestr[100];
        sprintf(framestr, "debug:avgframedur~%fms:fps~%f:frameno~%d:",avgdur(framedur),avgfps(),frameno++ );
        if (verbose && (frameno % 100 == 1))
            cout << framestr << endl;

    }
    
    cout << "track_targets complete, exiting" << endl;
    cout.flush();
    return 0;

}

