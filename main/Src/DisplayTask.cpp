/// @file DisplayTask.cpp
/// @brief Display task implementation (see DisplayTask.hpp).

#include "DisplayTask.hpp"
#include "Display.hpp"
#include "TaskMonitor.hpp"
#include "Utils.hpp"

#include <cstdio>

namespace
{
    constexpr uint32_t MSG_Q_LENGTH = 10;                   ///< Number of messages the queue can hold
    constexpr uint32_t CPU_UPDATE_PERIOD = 500;             ///< Period (ms) for updating the CPU usage
    constexpr uint32_t CHECKIN_TIMEOUT = 2 * MS_PER_SEC;    ///< Task monitor timeout (ms)

    constexpr const char* CPU_PLACEHOLDER = "CPU --%";      ///< Shown until the first CPU usage measurement
    constexpr int CPU_TEXT_LENGTH = 7;                      ///< Length of the CPU text, e.g. "CPU 25%" or "CPU100%"

    constexpr uint16_t TITLE_COLOR = Color::Rgb565(255, 140, 0);    ///< Title color: orange
    constexpr uint16_t TEXT_COLOR = Color::WHITE;                   ///< CPU usage color
    constexpr uint16_t BACKGROUND_COLOR = Color::BLACK;             ///< Background color

    //------------------------------------------------------------------
    // CenteredX
    //------------------------------------------------------------------
    /// @brief Get the left edge that centers a line of text horizontally
    /// @param[in] text - null-terminated text
    /// @return left edge, in pixels
    int CenteredX(const char* text)
    {
        return (Display::GetWidth() - Display::TextWidth(text, DisplayTask::TEXT_SCALE)) / 2;
    }
}

QHandle DisplayTask::msgQHandle = nullptr;

//------------------------------------------------------------------
// SendMessage
//------------------------------------------------------------------
void DisplayTask::SendMessage(Message::MsgId id)
{
    const Message msg(id);
    const StatusType status = TargetPort::QSend(msgQHandle, &msg);
    ASSERT(PASS == status);
    (void)status;
}

//------------------------------------------------------------------
// TaskCheckin
//------------------------------------------------------------------
void DisplayTask::TaskCheckin()
{
    SendMessage(Message::TASK_CHECKIN);
}

//------------------------------------------------------------------
// Shutdown
//------------------------------------------------------------------
void DisplayTask::Shutdown()
{
    SendMessage(Message::SHUTDOWN);
}

//------------------------------------------------------------------
// Initialize
//------------------------------------------------------------------
void DisplayTask::Initialize()
{
    msgQHandle = TargetPort::CreateQ(MSG_Q_LENGTH, sizeof(Message));
    ASSERT(nullptr != msgQHandle);
    SendMessage(Message::INITIALIZE);
}

//------------------------------------------------------------------
// Run
//------------------------------------------------------------------
void DisplayTask::Run()
{
    Message msg;
    uint32_t nextCpuUpdate = TargetPort::GetTimeMs() + CPU_UPDATE_PERIOD;

    while (true)
    {
        // Wait for a message, but only until the next CPU update is due
        const int32_t untilCpuUpdate = static_cast<int32_t>(nextCpuUpdate - TargetPort::GetTimeMs());
        const uint32_t wait = (untilCpuUpdate > 0) ? static_cast<uint32_t>(untilCpuUpdate) : 0;

        if (TRUE == TargetPort::QRecv(msgQHandle, &msg, MS_TO_TICKS(wait)))
        {
            switch (msg.msgId)
            {
                case Message::TASK_CHECKIN:
                    TaskMonitor::TaskCheckin();
                    break;

                case Message::INITIALIZE:
                    HandleInitialize();
                    break;

                case Message::SHUTDOWN:
                    HandleShutdown();
                    break;

                default:
                    printf("DisplayTask - unknown msgId: %d\r\n", msg.msgId);
                    break;
            }
        }

        if (static_cast<int32_t>(TargetPort::GetTimeMs() - nextCpuUpdate) >= 0)
        {
            UpdateCpuUsage();

            // Keep to the schedule. If an update was missed entirely, restart the schedule instead of catching up.
            nextCpuUpdate += CPU_UPDATE_PERIOD;
            if (static_cast<int32_t>(nextCpuUpdate - TargetPort::GetTimeMs()) <= 0)
            {
                nextCpuUpdate = TargetPort::GetTimeMs() + CPU_UPDATE_PERIOD;
            }
        }
    }
}

//------------------------------------------------------------------
// HandleInitialize
//------------------------------------------------------------------
void DisplayTask::HandleInitialize()
{
    // Register this task with the Task Monitor
    TaskMonitor::Register(CHECKIN_TIMEOUT, &TaskCheckin);

    Display::DrawString(CenteredX(TITLE), TITLE_Y, TITLE, TITLE_COLOR, BACKGROUND_COLOR, TEXT_SCALE);

    // Start measuring: the first reading is the starting point for the first update
    uint32_t idleTime = 0;
    uint32_t totalTime = 0;
    TargetPort::GetCpuTimes(idleTime, totalTime);
    cpuUsage.Update(idleTime, totalTime);
    DrawCpuText(CPU_PLACEHOLDER);

    printf("DisplayTask Initialized\r\n");
}

//------------------------------------------------------------------
// UpdateCpuUsage
//------------------------------------------------------------------
void DisplayTask::UpdateCpuUsage()
{
    uint32_t idleTime = 0;
    uint32_t totalTime = 0;
    TargetPort::GetCpuTimes(idleTime, totalTime);
    const uint32_t percent = cpuUsage.Update(idleTime, totalTime);

    if (cpuUsage.HasValue())
    {
        char text[CPU_TEXT_LENGTH + 1];
        snprintf(text, sizeof(text), "CPU%3lu%%", (unsigned long)percent);
        DrawCpuText(text);
    }
}

//------------------------------------------------------------------
// DrawCpuText
//------------------------------------------------------------------
void DisplayTask::DrawCpuText(const char* text)
{
    Display::DrawString(CenteredX(text), CPU_Y, text, TEXT_COLOR, BACKGROUND_COLOR, TEXT_SCALE);
}

//------------------------------------------------------------------
// HandleShutdown
//------------------------------------------------------------------
void DisplayTask::HandleShutdown()
{
    printf("DisplayTask Shutdown\r\n");
    // wait forever
    DELAY(MAX_DELAY);
}
