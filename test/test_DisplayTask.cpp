/// @file test_DisplayTask.cpp
/// @brief Host unit tests for DisplayTask.cpp.
/// @details Uses the real TaskMonitor, Display, TextRenderer, font and CpuUsage. Only the port layers are fakes: the
///          OS (fakes/TargetPort.hpp, which also simulates the CPU load) and DisplayPort (defined here).

#include "DisplayTask.hpp"

#include "Display.hpp"
#include "DisplayPort.hpp"
#include "StdoutCapture.hpp"
#include "TargetPort.hpp"
#include "TaskMonitor.hpp"
#include "TextRenderer.hpp"

#include "unity.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace
{
    constexpr int WIDTH = 240;      ///< Width of the fake panel (the Dial's)
    constexpr int HEIGHT = 240;     ///< Height of the fake panel (the Dial's)

    constexpr int SCALE = DisplayTask::TEXT_SCALE;                      ///< Text size used by DisplayTask
    constexpr int CELL_W = TextRenderer::CellWidth(SCALE, TextRenderer::Rotation::NONE);   ///< Character width (12)
    constexpr int CELL_H = TextRenderer::CellHeight(SCALE, TextRenderer::Rotation::NONE);  ///< Character height (16)

    constexpr uint16_t TITLE_COLOR = Color::Rgb565(255, 140, 0);    ///< Orange
    constexpr uint16_t CPU_COLOR = Color::WHITE;                    ///< White

    constexpr int TITLE_X = 48;     ///< "Heat Troller": 12 characters, 144 pixels, centered
    constexpr int CPU_X = 78;       ///< "CPU 25%": 7 characters, 84 pixels, centered

    /// @brief One DisplayPort::DrawBitmap() call
    struct Bitmap
    {
        int x;                          ///< Left edge
        int y;                          ///< Top edge
        int w;                          ///< Width
        int h;                          ///< Height
        std::vector<uint16_t> pixels;   ///< Copy of the pixels
        uint32_t timeMs;                ///< Fake time when it was drawn
    };

    std::vector<Bitmap> bitmaps;    ///< Every DrawBitmap() call, in order

    FakeTask displayTask{"DisplayTask"};    ///< The task running DisplayTask

    QHandle monitorQ = nullptr;     ///< The task monitor's queue
    QHandle displayQ = nullptr;     ///< DisplayTask's queue

    std::unique_ptr<DisplayTask> task;  ///< Task under test (kept across runs within a test, as it holds the usage)

    /// @brief Run the task
    /// @return everything printed to stdout
    std::string RunTask()
    {
        FakeTargetPort::currentTask = &displayTask;
        StdoutCapture capture;
        capture.Start();
        try
        {
            task->Run();
        }
        catch (const StopRun&)
        {
        }
        return capture.Stop();
    }

    /// @brief Run the task for a number of queue reads (each read gets a message, or waits for the next CPU update)
    /// @param[in] reads - number of queue reads
    /// @return everything printed to stdout
    std::string RunReads(int reads)
    {
        FakeTargetPort::recvBudget = reads;
        return RunTask();
    }

    /// @brief Run the task until the fake clock has advanced by a time
    /// @details The task is restarted, so its 500 ms CPU schedule starts now.
    /// @param[in] ms - time to run for
    /// @param[in] onRecv - optional, called at every queue read (e.g. to make a message arrive during a wait)
    /// @return everything printed to stdout
    std::string RunForMs(uint32_t ms, std::function<void()> onRecv = nullptr)
    {
        const uint32_t end = FakeTargetPort::timeMs + ms;
        FakeTargetPort::recvBudget = -1;
        FakeTargetPort::onRecv = [end, onRecv]()
        {
            if (onRecv)
            {
                onRecv();
            }
            if (FakeTargetPort::timeMs >= end)
            {
                throw StopRun();
            }
        };
        const std::string printed = RunTask();
        FakeTargetPort::onRecv = nullptr;
        return printed;
    }

    /// @brief Get the messages sent to the task monitor
    /// @return messages, oldest first
    std::vector<TaskMonitor::TaskMonitorMessage> MonitorMessages()
    {
        std::vector<TaskMonitor::TaskMonitorMessage> msgs;
        for (const FakeTargetPort::SentItem& item : FakeTargetPort::sent)
        {
            if (item.queue == monitorQ)
            {
                msgs.push_back(item.As<TaskMonitor::TaskMonitorMessage>());
            }
        }
        return msgs;
    }

    /// @brief Get the drawn characters on one text line
    /// @param[in] y - top edge of the line
    /// @return the bitmaps, in order
    std::vector<Bitmap> LineBitmaps(int y)
    {
        std::vector<Bitmap> result;
        for (const Bitmap& b : bitmaps)
        {
            if (b.y == y)
            {
                result.push_back(b);
            }
        }
        return result;
    }

    /// @brief Get the drawn characters of the CPU text
    /// @return the bitmaps, in order
    std::vector<Bitmap> CpuBitmaps()
    {
        return LineBitmaps(DisplayTask::CPU_Y);
    }

    /// @brief Check that a bitmap is one character as DisplayTask draws it
    /// @param[in] b - bitmap to check
    /// @param[in] c - expected character
    /// @param[in] x - expected left edge
    /// @param[in] y - expected top edge
    /// @param[in] fg - expected text color
    void CheckChar(const Bitmap& b, char c, int x, int y, uint16_t fg)
    {
        char msg[48];
        std::snprintf(msg, sizeof(msg), "'%c' at (%d, %d)", c, x, y);
        TEST_ASSERT_EQUAL_MESSAGE(x, b.x, msg);
        TEST_ASSERT_EQUAL_MESSAGE(y, b.y, msg);
        TEST_ASSERT_EQUAL_MESSAGE(CELL_W, b.w, msg);
        TEST_ASSERT_EQUAL_MESSAGE(CELL_H, b.h, msg);

        std::vector<uint16_t> expected(TextRenderer::CellPixels(SCALE));
        TextRenderer::RenderChar(c, fg, Color::BLACK, SCALE, expected.data());
        TEST_ASSERT_EQUAL_HEX16_ARRAY_MESSAGE(expected.data(), b.pixels.data(), expected.size(), msg);
    }

    /// @brief Check that some bitmaps are a line of text
    /// @param[in] drawn - bitmaps
    /// @param[in] first - index of the first character
    /// @param[in] text - expected text
    /// @param[in] x - expected left edge of the text
    /// @param[in] y - expected top edge of the text
    /// @param[in] fg - expected text color
    void CheckText(const std::vector<Bitmap>& drawn, size_t first, const char* text, int x, int y, uint16_t fg)
    {
        const size_t len = std::strlen(text);
        TEST_ASSERT_TRUE_MESSAGE(drawn.size() >= first + len, "text not drawn");
        for (size_t i = 0; i < len; i++)
        {
            CheckChar(drawn[first + i], text[i], x + (static_cast<int>(i) * CELL_W), y, fg);
        }
    }

    /// @brief Check that some bitmaps are the CPU text
    /// @param[in] drawn - bitmaps
    /// @param[in] first - index of the first character
    /// @param[in] text - expected 7 character text
    void CheckCpuText(const std::vector<Bitmap>& drawn, size_t first, const char* text)
    {
        CheckText(drawn, first, text, CPU_X, DisplayTask::CPU_Y, CPU_COLOR);
    }

    /// @brief Check that a rectangle is inside the round screen (all four corners within its 120 pixel radius)
    /// @param[in] x - left edge
    /// @param[in] y - top edge
    /// @param[in] w - width
    /// @param[in] h - height
    void CheckInsideCircle(int x, int y, int w, int h)
    {
        const int corners[4][2] = {{x, y}, {x + w, y}, {x, y + h}, {x + w, y + h}};
        for (const auto& corner : corners)
        {
            const int dx = corner[0] - (WIDTH / 2);
            const int dy = corner[1] - (HEIGHT / 2);
            TEST_ASSERT_LESS_OR_EQUAL(120 * 120, (dx * dx) + (dy * dy));
        }
    }

    /// @brief Run the INITIALIZE message (draws the title and the CPU placeholder)
    void RunInitialize()
    {
        RunReads(1);
        TEST_ASSERT_EQUAL(12 + 7, bitmaps.size());
    }
}

//------------------------------------------------------------------
// Fake DisplayPort
//------------------------------------------------------------------
void DisplayPort::Initialize() {}

void DisplayPort::BacklightOn() {}

int DisplayPort::GetWidth()
{
    return WIDTH;
}

int DisplayPort::GetHeight()
{
    return HEIGHT;
}

void DisplayPort::FillRect(int x, int y, int w, int h, uint16_t color)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)color;
}

void DisplayPort::DrawBitmap(int x, int y, int w, int h, const uint16_t* pixels)
{
    bitmaps.push_back(Bitmap{x, y, w, h, std::vector<uint16_t>(pixels, pixels + (w * h)), FakeTargetPort::timeMs});
}

/// @brief Unity: called before each test. Creates the task monitor's queue, then DisplayTask's (holding INITIALIZE).
void setUp(void)
{
    FakeTargetPort::Reset();
    bitmaps.clear();

    TaskMonitor::Initialize();
    monitorQ = FakeTargetPort::sent.back().queue;

    FakeTargetPort::currentTask = &displayTask;
    task = std::make_unique<DisplayTask>();
    task->Initialize();
    displayQ = FakeTargetPort::sent.back().queue;
}

/// @brief Unity: called after each test
void tearDown(void)
{
    task.reset();
}

//------------------------------------------------------------------
// Tests: task
//------------------------------------------------------------------

/// @brief Initialize creates the task's own queue and posts INITIALIZE to it
void test_Initialize_PostsInitialize(void)
{
    TEST_ASSERT_NOT_EQUAL(monitorQ, displayQ);
    TEST_ASSERT_EQUAL(1, FakeTargetPort::QueueCount(displayQ));
    TEST_ASSERT_EQUAL(Message::INITIALIZE, FakeTargetPort::sent.back().As<Message>().msgId);
}

/// @brief On INITIALIZE the task registers with the monitor: as itself, 2 s timeout, its TaskCheckin callback
void test_HandleInitialize_RegistersWithMonitor(void)
{
    const std::string printed = RunReads(1);

    const std::vector<TaskMonitor::TaskMonitorMessage> msgs = MonitorMessages();
    TEST_ASSERT_EQUAL(2, msgs.size());     // the monitor's own INITIALIZE, then REGISTER
    TEST_ASSERT_EQUAL(Message::REGISTER, msgs[1].msgId);
    TEST_ASSERT_EQUAL_PTR(&displayTask, msgs[1].taskId);
    TEST_ASSERT_EQUAL_UINT32(2000, msgs[1].maxResponseTime);
    TEST_ASSERT_EQUAL_PTR(&DisplayTask::TaskCheckin, msgs[1].checkinCall);
    TEST_ASSERT_EQUAL(1, StdoutCapture::Count(printed, "DisplayTask Initialized"));
}

/// @brief On INITIALIZE the title is drawn centered above the middle, in orange
void test_HandleInitialize_DrawsTitle(void)
{
    RunInitialize();

    CheckText(LineBitmaps(DisplayTask::TITLE_Y), 0, "Heat Troller", TITLE_X, DisplayTask::TITLE_Y, TITLE_COLOR);
}

/// @brief On INITIALIZE the CPU placeholder is drawn centered below the middle, in white
void test_HandleInitialize_DrawsCpuPlaceholder(void)
{
    RunInitialize();

    CheckCpuText(CpuBitmaps(), 0, "CPU --%");
}

/// @brief All text is inside the round screen
void test_Layout_InsideRoundScreen(void)
{
    CheckInsideCircle(TITLE_X, DisplayTask::TITLE_Y, 12 * CELL_W, CELL_H);
    CheckInsideCircle(CPU_X, DisplayTask::CPU_Y, 7 * CELL_W, CELL_H);
}

//------------------------------------------------------------------
// Tests: CPU usage
//------------------------------------------------------------------

/// @brief The CPU usage is redrawn every 500 ms with the measured load
void test_CpuUsage_UpdatedEvery500ms(void)
{
    RunInitialize();
    FakeTargetPort::cpuBusyPercent = 25;
    const uint32_t start = FakeTargetPort::timeMs;

    RunForMs(1500);

    const std::vector<Bitmap> cpu = CpuBitmaps();
    TEST_ASSERT_EQUAL(7 + (3 * 7), cpu.size());     // placeholder, then 3 updates
    for (int update = 0; update < 3; update++)
    {
        const size_t first = 7 + (update * 7);
        CheckCpuText(cpu, first, "CPU 25%");
        TEST_ASSERT_EQUAL_UINT32(start + (500 * (update + 1)), cpu[first].timeMs);
    }
}

/// @brief Each update shows the load since the previous update, including 0 % and 100 % (no space: "CPU100%")
void test_CpuUsage_FollowsLoad(void)
{
    RunInitialize();

    const uint32_t loads[] = {10, 80, 0, 100, 7};
    const char* texts[] = {"CPU 10%", "CPU 80%", "CPU  0%", "CPU100%", "CPU  7%"};
    for (size_t i = 0; i < 5; i++)
    {
        FakeTargetPort::cpuBusyPercent = loads[i];
        const size_t first = CpuBitmaps().size();
        RunForMs(500);  // one more update (the same task, so the usage continues from the last reading)
        CheckCpuText(CpuBitmaps(), first, texts[i]);
    }
}

/// @brief A checkin request is answered as the task, and doesn't push the next CPU update back
void test_CheckinRequest_DoesNotDelayUpdate(void)
{
    RunInitialize();
    const uint32_t start = FakeTargetPort::timeMs;

    // The checkin request arrives 200 ms into the wait for the first update
    bool sent = false;
    RunForMs(500, [&sent]()
    {
        if (!sent)
        {
            sent = true;
            FakeTargetPort::AdvanceTime(200);
            DisplayTask::TaskCheckin();
        }
    });

    const TaskMonitor::TaskMonitorMessage last = MonitorMessages().back();
    TEST_ASSERT_EQUAL(Message::TASK_CHECKIN, last.msgId);
    TEST_ASSERT_EQUAL_PTR(&displayTask, last.taskId);

    const std::vector<Bitmap> cpu = CpuBitmaps();
    TEST_ASSERT_EQUAL(7 + 7, cpu.size());
    TEST_ASSERT_EQUAL_UINT32(start + 500, cpu[7].timeMs);
}

/// @brief If the task was held up past an update, it updates once and restarts the schedule instead of catching up
void test_LateUpdate_DoesNotCatchUp(void)
{
    RunInitialize();
    const uint32_t start = FakeTargetPort::timeMs;

    // The task is held up for 1.7 s (more than 3 periods); a message then wakes it
    bool sent = false;
    RunForMs(2200, [&sent]()
    {
        if (!sent)
        {
            sent = true;
            FakeTargetPort::AdvanceTime(1700);
            DisplayTask::TaskCheckin();
        }
    });

    const std::vector<Bitmap> cpu = CpuBitmaps();
    TEST_ASSERT_EQUAL(7 + (2 * 7), cpu.size());         // two updates, not four
    TEST_ASSERT_EQUAL_UINT32(start + 1700, cpu[7].timeMs);   // straight away
    TEST_ASSERT_EQUAL_UINT32(start + 2200, cpu[14].timeMs);  // then 500 ms later
}

//------------------------------------------------------------------
// Tests: other messages
//------------------------------------------------------------------

/// @brief Unknown messages are reported and ignored
void test_UnknownMessage_Reported(void)
{
    RunInitialize();
    const Message bad(Message::NUM_MSG_IDS);
    TEST_ASSERT_EQUAL(PASS, TargetPort::QSend(displayQ, &bad));

    const std::string printed = RunReads(1);
    TEST_ASSERT_EQUAL(1, StdoutCapture::Count(printed, "DisplayTask - unknown msgId"));
}

/// @brief Shutdown prints a message and then blocks forever
void test_Shutdown_BlocksForever(void)
{
    RunInitialize();
    task->Shutdown();

    const std::string printed = RunReads(5);

    TEST_ASSERT_EQUAL(1, StdoutCapture::Count(printed, "DisplayTask Shutdown"));
    TEST_ASSERT_EQUAL(1, FakeTargetPort::delays.size());
    TEST_ASSERT_EQUAL_UINT32(MAX_DELAY, FakeTargetPort::delays[0]);
}

/// @brief Run all tests
/// @return number of failed tests (0 if all passed)
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_Initialize_PostsInitialize);
    RUN_TEST(test_HandleInitialize_RegistersWithMonitor);
    RUN_TEST(test_HandleInitialize_DrawsTitle);
    RUN_TEST(test_HandleInitialize_DrawsCpuPlaceholder);
    RUN_TEST(test_Layout_InsideRoundScreen);
    RUN_TEST(test_CpuUsage_UpdatedEvery500ms);
    RUN_TEST(test_CpuUsage_FollowsLoad);
    RUN_TEST(test_CheckinRequest_DoesNotDelayUpdate);
    RUN_TEST(test_LateUpdate_DoesNotCatchUp);
    RUN_TEST(test_UnknownMessage_Reported);
    RUN_TEST(test_Shutdown_BlocksForever);
    return UNITY_END();
}
