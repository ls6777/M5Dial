/// @file HelloTask.cpp
/// @brief Hello world task implementation (see HelloTask.hpp).

#include "HelloTask.hpp"
#include "PrintUtils.hpp"
#include "TaskMonitor.hpp"
#include "Utils.hpp"

#include <cstdio>

namespace
{
    constexpr uint32_t MSG_Q_LENGTH = 10;                   ///< Number of messages the queue can hold
    constexpr uint32_t HELLO_PERIOD = 1 * MS_PER_SEC;       ///< Period (ms) for printing the hello message
    constexpr uint32_t CHECKIN_TIMEOUT = 5 * MS_PER_SEC;    ///< Task monitor timeout (ms)
    constexpr uint32_t STATS_EVERY = 10;                    ///< Print the statistics every this many hellos

    /// @brief Size of the task statistics buffer, in bytes. TargetPort reserves 64 bytes per task, so this holds the
    ///        tables for up to 16 tasks.
    constexpr size_t TASK_STATS_SIZE = 1024;

    /// @brief Holds one task statistics table at a time (static: too large for the task's stack)
    char taskStatsBuf[TASK_STATS_SIZE];
}

QHandle HelloTask::msgQHandle = nullptr;

//------------------------------------------------------------------
// SendMessage
//------------------------------------------------------------------
void HelloTask::SendMessage(Message::MsgId id)
{
    const Message msg(id);
    const StatusType status = TargetPort::QSend(msgQHandle, &msg);
    ASSERT(PASS == status);
    (void)status;
}

//------------------------------------------------------------------
// TaskCheckin
//------------------------------------------------------------------
void HelloTask::TaskCheckin()
{
    SendMessage(Message::TASK_CHECKIN);
}

//------------------------------------------------------------------
// Shutdown
//------------------------------------------------------------------
void HelloTask::Shutdown()
{
    SendMessage(Message::SHUTDOWN);
}

//------------------------------------------------------------------
// Initialize
//------------------------------------------------------------------
void HelloTask::Initialize()
{
    msgQHandle = TargetPort::CreateQ(MSG_Q_LENGTH, sizeof(Message));
    ASSERT(nullptr != msgQHandle);
    SendMessage(Message::INITIALIZE);
}

//------------------------------------------------------------------
// Run
//------------------------------------------------------------------
void HelloTask::Run()
{
    StatusType status = FALSE;
    Message msg;

    while (true)
    {
        // Wait for new message up to HELLO_PERIOD
        status = TargetPort::QRecv(msgQHandle, &msg, MS_TO_TICKS(HELLO_PERIOD));

        if (TRUE == status)
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
                    printf("HelloTask - unknown msgId: %d\r\n", msg.msgId);
                    break;
            }
        }
        else
        {
            SayHello();
        }
    }
}

//------------------------------------------------------------------
// HandleInitialize
//------------------------------------------------------------------
void HelloTask::HandleInitialize()
{
    // Register this task with the Task Monitor
    TaskMonitor::Register(CHECKIN_TIMEOUT, &TaskCheckin);
    printf("HelloTask Initialized\r\n");
}

//------------------------------------------------------------------
// SayHello
//------------------------------------------------------------------
void HelloTask::SayHello()
{
    static uint32_t count = 0;

    count++;
    printf("Hello world! %lu\r\n", (unsigned long)count);

    if ((count % STATS_EVERY) == 0)
    {
        PrintTaskStats();
        printf("Printf buffer high water mark: %d of %d bytes\r\n", PrintUtils::GetHighWaterMark(),
               PrintUtils::BUFFER_SIZE - 1);
    }
}

//------------------------------------------------------------------
// PrintTaskStats
//------------------------------------------------------------------
void HelloTask::PrintTaskStats()
{
    // printf copies the text into the printf buffer, so one buffer can hold each table in turn
    TargetPort::GetTaskList(taskStatsBuf, sizeof(taskStatsBuf));
    printf("\r\nTASK INFO**********************************************\r\n");
    printf("Name            State  Priority  Stack  Num\r\n");
    printf("%s\r\n", taskStatsBuf);

    TargetPort::GetRunTimeStats(taskStatsBuf, sizeof(taskStatsBuf));
    printf("TASK STATS INFO****************************************\r\n");
    printf("Name            Abs Time (us)  %% Time\r\n");
    printf("%s\r\n", taskStatsBuf);
}

//------------------------------------------------------------------
// HandleShutdown
//------------------------------------------------------------------
void HelloTask::HandleShutdown()
{
    printf("HelloTask Shutdown\r\n");
    // wait forever
    DELAY(MAX_DELAY);
}
