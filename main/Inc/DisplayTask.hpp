#pragma once

/// @file DisplayTask.hpp
/// @brief Task that owns the LCD: the app title and the CPU usage.

#include "CpuUsage.hpp"
#include "Message.hpp"
#include "TargetPort.hpp"

/// @brief Display task
/// @details The only task that draws on the LCD (Display isn't thread safe). The Dial's screen is round (240 pixels
///          across), so everything is centered horizontally and kept away from the edge:
///          - the title ("Heat Troller") above the center, in orange
///          - the CPU usage ("CPU 25%") below the center, in white, updated every 500 ms
///
///          Registers with the task monitor with a 2 second timeout and checks in when the monitor asks.
class DisplayTask
{
    public:

        static constexpr const char* TITLE = "Heat Troller";   ///< App title shown on the screen
        static constexpr int TEXT_SCALE = 2;                    ///< Size of all text (12 x 16 pixel characters)
        static constexpr int TITLE_Y = 72;                      ///< Top edge of the title, in pixels
        static constexpr int CPU_Y = 152;                       ///< Top edge of the CPU usage, in pixels

        /// @brief Create this task's message queue and post the initialize message to it
        /// @note Must be called before Run(). Display::Initialize() must have been called.
        void Initialize();

        /// @brief Execution of DisplayTask
        /// @details Main loop: processes messages posted to the queue and updates the CPU usage every 500 ms. The CPU
        ///          updates keep to the 500 ms schedule however many messages arrive. Never returns.
        void Run();

        /// @brief Request a checkin from this task (task monitor checkin callback)
        /// @details Posts a checkin message to this task's queue. The task then checks in with the task monitor.
        static void TaskCheckin();

        /// @brief Post a message to shut down this task
        void Shutdown();

    private:

        /// @brief Post a message to this task's queue
        /// @param[in] id - ID of the message to send
        static void SendMessage(Message::MsgId id);

        /// @brief Handle the initialize message: register with the task monitor, draw the title, start measuring the
        ///        CPU usage and show a placeholder for it until the first measurement
        void HandleInitialize();

        /// @brief Measure the CPU usage since the last update and draw it
        void UpdateCpuUsage();

        /// @brief Draw the CPU usage text, centered
        /// @param[in] text - text to draw; always CPU_TEXT_LENGTH characters, so it covers the previous text
        void DrawCpuText(const char* text);

        /// @brief Handle the shutdown message: print a message and stop processing
        void HandleShutdown();

        CpuUsage cpuUsage;          ///< CPU usage calculation

        static QHandle msgQHandle;  ///< This task's message queue
};
