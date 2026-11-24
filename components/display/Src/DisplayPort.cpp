/// @file DisplayPort.cpp
/// @brief DisplayPort implementation for the M5Stack Dial's 1.28" 240x240 round LCD (GC9A01, ESP-IDF SPI master
///        driver).
/// @details Pins, SPI host, color inversion and the controller initialization sequence match M5Stack's M5GFX library
///          (board_M5Dial, Panel_GC9A01). The panel is write only: it has no MISO line.
///
///          The panel is used in its native orientation (rotation 0, the M5GFX default for the Dial). It is round, so
///          only the pixels inside the 120 pixel radius circle around (120, 120) are visible; the corners of the
///          240 x 240 memory are hidden. Pixels are sent as big-endian RGB565, so they're byte swapped into a DMA
///          capable buffer before sending.

#include "DisplayPort.hpp"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cassert>

namespace
{
    constexpr spi_host_device_t LCD_HOST = SPI2_HOST;   ///< SPI peripheral connected to the LCD

    constexpr gpio_num_t PIN_MOSI = GPIO_NUM_5;         ///< SPI MOSI
    constexpr gpio_num_t PIN_CLK = GPIO_NUM_6;          ///< SPI clock
    constexpr gpio_num_t PIN_CS = GPIO_NUM_7;           ///< SPI chip select
    constexpr gpio_num_t PIN_DC = GPIO_NUM_4;           ///< Data / command select (low = command)
    constexpr gpio_num_t PIN_RST = GPIO_NUM_8;          ///< Controller reset (active low)
    constexpr gpio_num_t PIN_BCKL = GPIO_NUM_9;         ///< Backlight control
    constexpr int BACKLIGHT_ON_LEVEL = 1;               ///< The backlight is on when the pin is high

    /// @brief SPI clock. M5GFX runs this panel at 80 MHz; 40 MHz leaves margin while bringing the board up.
    constexpr int SPI_CLOCK_HZ = 40 * 1000 * 1000;

    constexpr int WIDTH = 240;                          ///< Panel width, in pixels
    constexpr int HEIGHT = 240;                         ///< Panel height, in pixels

    constexpr int BUF_PIXELS = WIDTH * 8;               ///< Pixels sent per SPI transfer (8 full rows)

    constexpr uint8_t CMD_COLUMN_SET = 0x2A;            ///< Column address set
    constexpr uint8_t CMD_PAGE_SET = 0x2B;              ///< Page (row) address set
    constexpr uint8_t CMD_MEMORY_WRITE = 0x2C;          ///< Memory write (pixel data follows)

    constexpr uint8_t DELAY_AFTER = 0x80;               ///< Init command flag: wait INIT_DELAY_MS after the command
    constexpr uint32_t INIT_DELAY_MS = 120;             ///< Wait after sleep out and display on
    constexpr uint8_t END_OF_CMDS = 0xFF;               ///< Init command list terminator

    /// @brief One command of a controller initialization sequence
    struct InitCmd
    {
        uint8_t cmd;            ///< Command byte
        uint8_t data[16];       ///< Parameter bytes
        uint8_t dataBytes;      ///< Number of parameter bytes, ORed with DELAY_AFTER, or END_OF_CMDS
    };

    /// @brief GC9A01 initialization: the M5GFX Panel_GC9A01 sequence, plus the memory access control, pixel format
    ///        and color inversion that M5GFX sends separately (rotation 0, BGR order, 16 bits per pixel, inverted)
    DRAM_ATTR const InitCmd GC9A01_INIT[] =
    {
        {0xEF, {0}, 0},                     // Inter register enable 2
        {0xEB, {0x14}, 1},
        {0xFE, {0}, 0},                     // Inter register enable 1
        {0xEF, {0}, 0},                     // Inter register enable 2
        {0xEB, {0x14}, 1},
        {0x84, {0x40}, 1},
        {0x85, {0xFF}, 1},
        {0x86, {0xFF}, 1},
        {0x87, {0xFF}, 1},
        {0x8E, {0xFF}, 1},
        {0x8F, {0xFF}, 1},
        {0x88, {0x0A}, 1},
        {0x89, {0x21}, 1},
        {0x8A, {0x00}, 1},
        {0x8B, {0x80}, 1},
        {0x8C, {0x01}, 1},
        {0x8D, {0x01}, 1},
        {0xB6, {0x00, 0x20}, 2},            // Display function control
        {0x90, {0x08, 0x08, 0x08, 0x08}, 4},
        {0xBD, {0x06}, 1},
        {0xBC, {0x00}, 1},
        {0xFF, {0x60, 0x01, 0x04}, 3},
        {0xC3, {0x13}, 1},                  // Power control 2
        {0xC4, {0x13}, 1},                  // Power control 3
        {0xC9, {0x22}, 1},                  // Power control 4
        {0xBE, {0x11}, 1},
        {0xE1, {0x10, 0x0E}, 2},
        {0xDF, {0x21, 0x0C, 0x02}, 3},
        {0xF0, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6},    // Gamma 1
        {0xF1, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6},    // Gamma 2
        {0xF2, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6},    // Gamma 3
        {0xF3, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6},    // Gamma 4
        {0xED, {0x1B, 0x0B}, 2},
        {0xAE, {0x77}, 1},
        {0xCD, {0x63}, 1},
        {0x70, {0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03}, 9},
        {0xE8, {0x34}, 1},                  // Frame rate
        {0x62, {0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70}, 12},
        {0x63, {0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70}, 12},
        {0x64, {0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07}, 7},
        {0x66, {0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00}, 10},
        {0x67, {0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98}, 10},
        {0x74, {0x10, 0x68, 0x80, 0x00, 0x00, 0x4E, 0x00}, 7},
        {0x98, {0x3E, 0x07}, 2},
        {0x35, {0x00}, 1},                  // Tearing effect line on
        {0x36, {0x08}, 1},                  // Memory access control: rotation 0, BGR order
        {0x3A, {0x05}, 1},                  // Pixel format: 16 bits per pixel
        {0x21, {0}, 0},                     // Display inversion on (the Dial's panel needs it for true colors)
        {0x11, {0}, DELAY_AFTER},           // Sleep out
        {0x29, {0}, DELAY_AFTER},           // Display on
        {0, {0}, END_OF_CMDS}
    };

    spi_device_handle_t spi = nullptr;      ///< The LCD on the SPI bus

    /// @brief Pixels being sent, already byte swapped (must be DMA capable)
    DMA_ATTR uint16_t pixelBuf[BUF_PIXELS];

    //------------------------------------------------------------------
    // PreTransferCallback
    //------------------------------------------------------------------
    /// @brief Called by the SPI driver (in interrupt context) just before each transfer, to set the D/C line
    /// @param[in] t - transfer about to start. Its user field holds the D/C level (0 = command, 1 = data).
    void IRAM_ATTR PreTransferCallback(spi_transaction_t* t)
    {
        gpio_set_level(PIN_DC, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(t->user)));
    }

    //------------------------------------------------------------------
    // Transmit
    //------------------------------------------------------------------
    /// @brief Send a transfer and wait for it to finish
    /// @param[in] t - transfer to send
    void Transmit(spi_transaction_t& t)
    {
        const esp_err_t err = spi_device_polling_transmit(spi, &t);
        assert(ESP_OK == err);
        (void)err;
    }

    //------------------------------------------------------------------
    // SendCmd
    //------------------------------------------------------------------
    /// @brief Send a command byte
    /// @param[in] cmd - command to send
    void SendCmd(uint8_t cmd)
    {
        spi_transaction_t t = {};
        t.length = 8;
        t.flags = SPI_TRANS_USE_TXDATA;
        t.tx_data[0] = cmd;
        t.user = reinterpret_cast<void*>(0);
        Transmit(t);
    }

    //------------------------------------------------------------------
    // SendData
    //------------------------------------------------------------------
    /// @brief Send parameter or pixel data
    /// @param[in] data - data to send (must be DMA capable if longer than 4 bytes)
    /// @param[in] len - length of data, in bytes
    void SendData(const void* data, size_t len)
    {
        if (len == 0)
        {
            return;
        }

        spi_transaction_t t = {};
        t.length = len * 8;
        t.tx_buffer = data;
        t.user = reinterpret_cast<void*>(1);
        Transmit(t);
    }

    //------------------------------------------------------------------
    // SetWindow
    //------------------------------------------------------------------
    /// @brief Set the rectangle that the next memory write fills, and start the memory write
    /// @param[in] x - left edge, in pixels
    /// @param[in] y - top edge, in pixels
    /// @param[in] w - width, in pixels
    /// @param[in] h - height, in pixels
    void SetWindow(int x, int y, int w, int h)
    {
        assert((x >= 0) && (y >= 0) && (w > 0) && (h > 0) && ((x + w) <= WIDTH) && ((y + h) <= HEIGHT));

        const int x2 = x + w - 1;
        const int y2 = y + h - 1;

        const uint8_t columns[4] = {static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(x),
                                    static_cast<uint8_t>(x2 >> 8), static_cast<uint8_t>(x2)};
        const uint8_t pages[4] = {static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(y),
                                  static_cast<uint8_t>(y2 >> 8), static_cast<uint8_t>(y2)};

        SendCmd(CMD_COLUMN_SET);
        SendData(columns, sizeof(columns));
        SendCmd(CMD_PAGE_SET);
        SendData(pages, sizeof(pages));
        SendCmd(CMD_MEMORY_WRITE);
    }

    //------------------------------------------------------------------
    // Swap
    //------------------------------------------------------------------
    /// @brief Convert an RGB565 pixel to the panel's byte order (big-endian)
    /// @param[in] color - RGB565 color
    /// @return color with its bytes swapped
    constexpr uint16_t Swap(uint16_t color)
    {
        return static_cast<uint16_t>((color << 8) | (color >> 8));
    }
}

//------------------------------------------------------------------
// Initialize
//------------------------------------------------------------------
void DisplayPort::Initialize()
{
    spi_bus_config_t busCfg = {};
    busCfg.mosi_io_num = PIN_MOSI;
    busCfg.miso_io_num = -1;
    busCfg.sclk_io_num = PIN_CLK;
    busCfg.quadwp_io_num = -1;
    busCfg.quadhd_io_num = -1;
    busCfg.max_transfer_sz = sizeof(pixelBuf);
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &busCfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devCfg = {};
    devCfg.clock_speed_hz = SPI_CLOCK_HZ;
    devCfg.mode = 0;
    devCfg.spics_io_num = PIN_CS;
    devCfg.queue_size = 1;
    devCfg.pre_cb = PreTransferCallback;
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &devCfg, &spi));

    gpio_config_t ioCfg = {};
    ioCfg.pin_bit_mask = (1ULL << PIN_DC) | (1ULL << PIN_RST) | (1ULL << PIN_BCKL);
    ioCfg.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&ioCfg));

    // Backlight off until the screen has been cleared
    gpio_set_level(PIN_BCKL, !BACKLIGHT_ON_LEVEL);

    // Reset the controller
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    for (const InitCmd* cmds = GC9A01_INIT; cmds->dataBytes != END_OF_CMDS; cmds++)
    {
        SendCmd(cmds->cmd);
        SendData(cmds->data, cmds->dataBytes & 0x1F);
        if (cmds->dataBytes & DELAY_AFTER)
        {
            vTaskDelay(pdMS_TO_TICKS(INIT_DELAY_MS));
        }
    }
}

//------------------------------------------------------------------
// BacklightOn
//------------------------------------------------------------------
void DisplayPort::BacklightOn()
{
    gpio_set_level(PIN_BCKL, BACKLIGHT_ON_LEVEL);
}

//------------------------------------------------------------------
// GetWidth
//------------------------------------------------------------------
int DisplayPort::GetWidth()
{
    return WIDTH;
}

//------------------------------------------------------------------
// GetHeight
//------------------------------------------------------------------
int DisplayPort::GetHeight()
{
    return HEIGHT;
}

//------------------------------------------------------------------
// FillRect
//------------------------------------------------------------------
void DisplayPort::FillRect(int x, int y, int w, int h, uint16_t color)
{
    SetWindow(x, y, w, h);

    int remaining = w * h;
    const int fillLen = (remaining < BUF_PIXELS) ? remaining : BUF_PIXELS;
    const uint16_t swapped = Swap(color);
    for (int i = 0; i < fillLen; i++)
    {
        pixelBuf[i] = swapped;
    }

    while (remaining > 0)
    {
        const int len = (remaining < BUF_PIXELS) ? remaining : BUF_PIXELS;
        SendData(pixelBuf, static_cast<size_t>(len) * sizeof(uint16_t));
        remaining -= len;
    }
}

//------------------------------------------------------------------
// DrawBitmap
//------------------------------------------------------------------
void DisplayPort::DrawBitmap(int x, int y, int w, int h, const uint16_t* pixels)
{
    assert(nullptr != pixels);

    SetWindow(x, y, w, h);

    int remaining = w * h;
    while (remaining > 0)
    {
        const int len = (remaining < BUF_PIXELS) ? remaining : BUF_PIXELS;
        for (int i = 0; i < len; i++)
        {
            pixelBuf[i] = Swap(pixels[i]);
        }
        SendData(pixelBuf, static_cast<size_t>(len) * sizeof(uint16_t));
        pixels += len;
        remaining -= len;
    }
}
