#define _USE_MATH_DEFINES
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <memory>
#include <iostream>
#include <windows.h>
#pragma warning(push, 0)
#include <opencv2/opencv.hpp>
#pragma warning(pop)
#include <cmath>

const struct CaptureConfig
{
    int x, y, radius;
} CAPTURE_REGION = {960, 568, 110};

const struct Color
{
    uint8_t r, g, b;
} TARGET_COLOR = {223, 223, 223},
  CURSOR_COLOR = {82, 91, 109};

struct Point
{
    int x, y;
};

struct SharedData
{
    std::atomic<bool> isInCursorLoop{false};
    std::atomic<bool> isMining{false};
};

const int r = CAPTURE_REGION.radius;
const int W = 2 * r, H = 2 * r;
cv::Mat mask(H, W, CV_8UC1);

class ScreenCapture
{
public:
    ScreenCapture(HWND hwnd, int w, int h)
        : hwnd(hwnd), width(w), height(h)
    {
        if (!hwnd)
            throw std::runtime_error("Invalid HWND");
        hWindowDC = GetDC(hwnd);
        hMemoryDC = CreateCompatibleDC(hWindowDC);

        ZeroMemory(&bmi, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        hBitmap = CreateDIBSection(hWindowDC,
                                   reinterpret_cast<BITMAPINFO *>(&bmi),
                                   DIB_RGB_COLORS,
                                   reinterpret_cast<void **>(&bufferPtr),
                                   NULL, 0);
        if (!hBitmap)
            throw std::runtime_error("CreateDIBSection failed!");
        SelectObject(hMemoryDC, hBitmap);

        img = cv::Mat(height, width, CV_8UC4, bufferPtr);
    }

    ~ScreenCapture()
    {
        if (hBitmap)
            DeleteObject(hBitmap);
        if (hMemoryDC)
            DeleteDC(hMemoryDC);
        if (hWindowDC)
            ReleaseDC(hwnd, hWindowDC);
    }

    const cv::Mat &capture(int x, int y)
    {
        BitBlt(hMemoryDC, 0, 0, width, height, hWindowDC, x, y, SRCCOPY);
        return img;
    }

private:
    HWND hwnd;
    HDC hWindowDC = nullptr;
    HDC hMemoryDC = nullptr;
    HBITMAP hBitmap = nullptr;
    BITMAPINFO bmi;
    int width, height;
    uint8_t *bufferPtr = nullptr;
    cv::Mat img;
};

static bool fastDetectBounds(const cv::Mat &imgBGRA,
                             const Color &c,
                             Point &minP, Point &maxP,
                             cv::Mat &mask)
{
    const uint8_t *data = imgBGRA.data;
    const size_t step = imgBGRA.step;
    uint8_t *maskData = mask.data;
    const size_t maskStep = mask.step;
    const uint32_t target = (c.b) | (c.g << 8) | (c.r << 16);
    const uint32_t maskColor = 0x00FFFFFF;

    for (int y = 0; y < imgBGRA.rows; ++y)
    {
        const uint32_t *row = reinterpret_cast<const uint32_t *>(data + y * step);
        uint8_t *maskRow = maskData + y * maskStep;
        for (int x = 0; x < imgBGRA.cols; ++x)
        {
            maskRow[x] = ((row[x] & maskColor) == target) ? 255 : 0;
        }
    }

    if (cv::countNonZero(mask) == 0)
        return false;

    cv::Rect bb = cv::boundingRect(mask);
    minP = {bb.x, bb.y};
    maxP = {bb.x + bb.width - 1, bb.y + bb.height - 1};
    return true;
}

static bool fastDetectColorPresence(const cv::Mat &imgBGRA, const Color &c)
{
    const uint8_t *data = imgBGRA.data;
    const size_t step = imgBGRA.step;
    const uint32_t target = (c.b) | (c.g << 8) | (c.r << 16);
    const uint32_t mask = 0x00FFFFFF;

    for (int y = 0; y < imgBGRA.rows; ++y)
    {
        const uint32_t *row = reinterpret_cast<const uint32_t *>(data + y * step);
        for (int x = 0; x < imgBGRA.cols; ++x)
        {
            if ((row[x] & mask) == target)
            {
                return true;
            }
        }
    }
    return false;
}

static void detection(SharedData &sd, std::atomic<bool> &run)
{
    const int ox = CAPTURE_REGION.x - r;
    const int oy = CAPTURE_REGION.y - r;

    std::unique_ptr<ScreenCapture> capFull;
    HWND lastHwnd = nullptr;
    bool cursorClicked = false;

    while (run)
    {
        HWND hwnd = FindWindow(NULL, L"Roblox");
        if (hwnd != lastHwnd)
        {
            capFull.reset();
            lastHwnd = hwnd;
        }

        if (!hwnd || GetForegroundWindow() != hwnd)
        {
            sd.isMining = false;
            sd.isInCursorLoop = false;
            cursorClicked = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (!capFull)
            capFull = std::make_unique<ScreenCapture>(hwnd, W, H);

        const cv::Mat &frame = capFull->capture(ox, oy);
        sd.isMining = false;
        sd.isInCursorLoop = false;

        Point tgtMin{}, tgtMax{};
        if (fastDetectBounds(frame, TARGET_COLOR, tgtMin, tgtMax, mask))
        {
            sd.isMining = true;
            int w = tgtMax.x - tgtMin.x + 1;
            int h = tgtMax.y - tgtMin.y + 1;
            cv::Rect roiRect(tgtMin.x, tgtMin.y, w, h);
            cv::Mat cursorImg = frame(roiRect);

            if (fastDetectColorPresence(cursorImg, CURSOR_COLOR))
            {
                if (!cursorClicked)
                {
                    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
                    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
                    cursorClicked = true;
                }
                sd.isInCursorLoop = true;
            }
            else
            {
                cursorClicked = false;
            }
        }
        else
        {
            cursorClicked = false;
        }
    }
}

int main()
{
    SharedData sd;
    std::atomic<bool> running{true};
    std::thread(detection, std::ref(sd), std::ref(running)).detach();

    std::cout << "Press Enter to stop...\n";
    std::cin.get();
    running = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return 0;
}