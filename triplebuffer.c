/**
 * Triple-Buffer LVGL Display Driver for ESP32-S3
 * 720x720 RGB565 Parallel (RGB) Display
 * 
 * Concept:
 *   Work Buffer (PSRAM)  - LVGL renders here incrementally, never displayed
 *   Back Buffer (PSRAM)  - Receives copy from Work Buffer via GDMA
 *   Front Buffer (PSRAM) - Currently displayed by LCD_CAM peripheral
 * 
 * Flow:
 *   1. LVGL renders incrementally into Work Buffer (multiple flush_cb calls)
 *   2. When LVGL is done (lv_disp_flush_is_last): 
 *      → GDMA copies Work Buffer → Back Buffer (non-blocking)
 *   3. When GDMA is done: 
 *      → Pointer swap: Back ↔ Front
 *      → LCD_CAM immediately displays the new Front Buffer
 *   4. LVGL can immediately start rendering the next frame into Work Buffer
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_async_memcpy.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "lvgl.h"

static const char *TAG = "triple_buf";

/* ============================================================
 * Configuration
 * ============================================================ */
#define DISP_WIDTH      720
#define DISP_HEIGHT     720
#define DISP_BPP        2       // RGB565 = 2 bytes per pixel
#define FB_SIZE         (DISP_WIDTH * DISP_HEIGHT * DISP_BPP)  // ~1 MB
#define FB_ALIGN        64      // Cache-line alignment for PSRAM DMA

/* ============================================================
 * Global Variables
 * ============================================================ */

// The three buffers
static uint8_t *work_buf  = NULL;   // LVGL renders into this
static uint8_t *front_buf = NULL;   // LCD_CAM reads from this
static uint8_t *back_buf  = NULL;   // GDMA copy target, then swap

// GDMA async memcpy
static async_memcpy_handle_t s_mcp_handle = NULL;
static SemaphoreHandle_t     s_copy_done_sem = NULL;
static volatile bool         s_copy_in_progress = false;

// LCD Panel Handle
static esp_lcd_panel_handle_t s_panel_handle = NULL;

// LVGL Display
static lv_disp_drv_t  s_disp_drv;
static lv_disp_draw_buf_t s_draw_buf;

/* ============================================================
 * GDMA Async Memcpy
 * ============================================================ */

/**
 * ISR Callback - called when GDMA copy is complete
 */
static IRAM_ATTR bool gdma_copy_done_cb(async_memcpy_handle_t hdl,
                                         async_memcpy_event_t *event,
                                         void *cb_args)
{
    BaseType_t high_task_wakeup = pdFALSE;
    xSemaphoreGiveFromISR(s_copy_done_sem, &high_task_wakeup);
    return (high_task_wakeup == pdTRUE);
}

/**
 * Initialize GDMA driver
 */
static esp_err_t gdma_copy_init(void)
{
    s_copy_done_sem = xSemaphoreCreateBinary();
    if (!s_copy_done_sem) return ESP_ERR_NO_MEM;

    async_memcpy_config_t config = ASYNC_MEMCPY_DEFAULT_CONFIG();
    config.backlog = 4;
    // AHB GDMA for PSRAM access on ESP32-S3
    return esp_async_memcpy_install_gdma_ahb(&config, &s_mcp_handle);
}

/**
 * Copy buffer via GDMA (non-blocking, then waits on semaphore)
 */
static void gdma_copy_buffer(void *dst, const void *src, size_t len)
{
    s_copy_in_progress = true;
    
    esp_err_t ret = esp_async_memcpy(s_mcp_handle, dst, (void *)src, len,
                                      gdma_copy_done_cb, NULL);
    if (ret != ESP_OK) {
        // Fallback: CPU memcpy
        ESP_LOGW(TAG, "GDMA copy failed (0x%x), falling back to memcpy", ret);
        memcpy(dst, src, len);
        s_copy_in_progress = false;
        return;
    }
    
    // Wait until DMA is done (blocks this task, but CPU is free for other tasks)
    xSemaphoreTake(s_copy_done_sem, portMAX_DELAY);
    s_copy_in_progress = false;
}

/* ============================================================
 * Buffer Swap
 * ============================================================ */

/**
 * Swap back and front buffer.
 * After this, LCD_CAM displays the new front buffer.
 */
static void swap_buffers(void)
{
    uint8_t *tmp = front_buf;
    front_buf = back_buf;
    back_buf = tmp;

    // Tell LCD_CAM panel about the new framebuffer
    // For esp_lcd_rgb_panel: the next VSYNC picks up the new buffer
    esp_lcd_panel_draw_bitmap(s_panel_handle, 0, 0, DISP_WIDTH, DISP_HEIGHT, front_buf);
}

/* ============================================================
 * LVGL Flush Callback
 * ============================================================ */

/**
 * LVGL calls this callback for each rendered region.
 * 
 * Since we use a full-frame Work Buffer, LVGL copies its
 * internal render results directly into the Work Buffer.
 * 
 * When the last flush of a frame arrives:
 *   1. GDMA: Copy Work → Back
 *   2. Pointer swap: Back ↔ Front
 */
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, 
                           lv_color_t *color_map)
{
    // LVGL hat einen Streifen im schnellen internen RAM gerendert.
    // Jetzt kopieren wir nur diesen Streifen in den PSRAM Work Buffer.
    
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    
    // Zeilenweise in den Work Buffer (PSRAM) kopieren
    for (int y = 0; y < h; y++) {
        uint16_t *src = (uint16_t *)color_map + y * w;
        uint16_t *dst = (uint16_t *)work_buf + 
                        ((area->y1 + y) * DISP_WIDTH + area->x1);
        memcpy(dst, src, w * sizeof(uint16_t));
    }

    if (lv_disp_flush_is_last(drv)) {
        // Frame komplett → GDMA copy work → back, dann swap
        gdma_copy_buffer(back_buf, work_buf, FB_SIZE);
        swap_buffers();
    }

    lv_disp_flush_ready(drv);
}
```

## Der Unterschied
```
Direct Mode (vorher):
  LVGL → random writes in PSRAM (langsam!) → GDMA → swap

Partial Mode (besser):
  LVGL → random writes in internem SRAM (schnell!)
       → sequentieller memcpy in PSRAM (cache-freundlich)
       → GDMA → swap

/* ============================================================
 * Buffer Allocation
 * ============================================================ */

static esp_err_t allocate_buffers(void)
{
    // All 3 buffers in PSRAM, cache-aligned
    work_buf = (uint8_t *)heap_caps_aligned_alloc(FB_ALIGN, FB_SIZE, MALLOC_CAP_SPIRAM);
    front_buf = (uint8_t *)heap_caps_aligned_alloc(FB_ALIGN, FB_SIZE, MALLOC_CAP_SPIRAM);
    back_buf = (uint8_t *)heap_caps_aligned_alloc(FB_ALIGN, FB_SIZE, MALLOC_CAP_SPIRAM);

    if (!work_buf || !front_buf || !back_buf) {
        ESP_LOGE(TAG, "Buffer allocation failed! Need 3x %d bytes PSRAM", FB_SIZE);
        return ESP_ERR_NO_MEM;
    }

    // Clear buffers (black screen)
    memset(work_buf, 0, FB_SIZE);
    memset(front_buf, 0, FB_SIZE);
    memset(back_buf, 0, FB_SIZE);

    ESP_LOGI(TAG, "3 framebuffers allocated: %d bytes each (%.1f MB total)", 
             FB_SIZE, (3.0f * FB_SIZE) / (1024.0f * 1024.0f));
    
    return ESP_OK;
}

/* ============================================================
 * LCD RGB Panel Setup (adjust to your display!)
 * ============================================================ */

static esp_err_t lcd_panel_init(void)
{
    ESP_LOGI(TAG, "Initializing RGB LCD panel...");

    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 24 * 1000 * 1000,   // 24 MHz pixel clock
            .h_res = DISP_WIDTH,
            .v_res = DISP_HEIGHT,
            // Adjust timing values to your display!
            .hsync_back_porch = 20,
            .hsync_front_porch = 40,
            .hsync_pulse_width = 2,
            .vsync_back_porch = 8,
            .vsync_front_porch = 20,
            .vsync_pulse_width = 2,
            .flags = {
                .pclk_active_neg = true,
            },
        },
        .data_width = 16,   // 16-bit parallel RGB565
        .num_fbs = 0,       // IMPORTANT: We manage buffers ourselves!
        .bounce_buffer_size_px = 0,
        // Adjust pin configuration to your board!
        .hsync_gpio_num = -1,   // TODO: Your pins
        .vsync_gpio_num = -1,   // TODO: Your pins
        .de_gpio_num = -1,      // TODO: Your pins
        .pclk_gpio_num = -1,    // TODO: Your pins
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            // TODO: Your 16 data pins here
            -1, -1, -1, -1, -1, -1, -1, -1,
            -1, -1, -1, -1, -1, -1, -1, -1,
        },
        .flags = {
            .fb_in_psram = 0,   // We manage buffers ourselves
        },
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&panel_config, &s_panel_handle),
                        TAG, "RGB panel creation failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel_handle), TAG, "Panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel_handle), TAG, "Panel init failed");

    return ESP_OK;
}

/* ============================================================
 * LVGL Setup
 * ============================================================ */

// Kleiner Buffer im schnellen internen SRAM
// 720 x 40 Zeilen = 57.600 Bytes → passt ins interne RAM
#define BUF_LINES  40
static lv_color_t *render_buf = NULL;

static void lvgl_display_init(void)
{
    lv_init();

    // Render-Buffer im schnellen internen RAM!
    render_buf = (lv_color_t *)heap_caps_malloc(
        DISP_WIDTH * BUF_LINES * sizeof(lv_color_t), 
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA
    );

    lv_disp_draw_buf_init(&s_draw_buf,
                           render_buf,               // Schneller interner Buffer
                           NULL,
                           DISP_WIDTH * BUF_LINES);  // Nicht full-frame!

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = DISP_WIDTH;
    s_disp_drv.ver_res = DISP_HEIGHT;
    s_disp_drv.flush_cb = lvgl_flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    
    // KEIN direct_mode → LVGL rendert in den kleinen internen Buffer
    s_disp_drv.direct_mode = 0;
    s_disp_drv.full_refresh = 0;

    lv_disp_drv_register(&s_disp_drv);
}

/* ============================================================
 * LVGL Task
 * ============================================================ */

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL task started on core %d", xPortGetCoreID());
    
    uint32_t frame_count = 0;
    TickType_t last_fps_tick = xTaskGetTickCount();

    while (1) {
        // LVGL timer handler - renders dirty areas into the Work Buffer
        uint32_t time_till_next = lv_timer_handler();
        
        // FPS logging every 5 seconds
        frame_count++;
        TickType_t now = xTaskGetTickCount();
        if ((now - last_fps_tick) >= pdMS_TO_TICKS(5000)) {
            float fps = (float)frame_count / 5.0f;
            ESP_LOGI(TAG, "FPS: %.1f", fps);
            frame_count = 0;
            last_fps_tick = now;
        }

        // LVGL wants to be called again in time_till_next ms
        // Minimum 1ms, maximum 10ms for smooth animations
        uint32_t delay = (time_till_next < 1) ? 1 : 
                         (time_till_next > 10) ? 10 : time_till_next;
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

/* ============================================================
 * Demo UI (your rotating pointer)
 * ============================================================ */

static void create_demo_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    // Example: A label that rotates / updates
    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "Triple Buffer Test");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
    lv_obj_center(label);

    // TODO: Add your rotating pointer (50x360) here
    // e.g. using lv_img + lv_img_set_angle() animation
}

/* ============================================================
 * Main
 * ============================================================ */

void app_main(void)
{
    ESP_LOGI(TAG, "=== Triple-Buffer LVGL Display Driver ===");
    ESP_LOGI(TAG, "Display: %dx%d RGB565 Parallel", DISP_WIDTH, DISP_HEIGHT);
    ESP_LOGI(TAG, "Buffer: 3x %.1f MB = %.1f MB PSRAM", 
             FB_SIZE / (1024.0f * 1024.0f), 3.0f * FB_SIZE / (1024.0f * 1024.0f));

    // 1. Allocate buffers
    ESP_ERROR_CHECK(allocate_buffers());

    // 2. Initialize GDMA
    ESP_ERROR_CHECK(gdma_copy_init());

    // 3. Initialize LCD panel
    ESP_ERROR_CHECK(lcd_panel_init());

    // 4. Display first frame (black)
    esp_lcd_panel_draw_bitmap(s_panel_handle, 0, 0, DISP_WIDTH, DISP_HEIGHT, front_buf);

    // 5. Initialize LVGL
    lvgl_display_init();

    // 6. Create demo UI
    create_demo_ui();

    // 7. Start LVGL task (Core 1, so Core 0 stays free)
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "System running!");
}
