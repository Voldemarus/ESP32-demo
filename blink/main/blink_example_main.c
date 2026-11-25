/* Blink Example with WiFi STA and LED error indication
 *
 * Индикация ошибок Wi-Fi внешним светодиодом (BLINK_GPIO_2):
 *   Число миганий (интервал 0.3 с, затем пауза 2 с):
 *     1 — Неизвестная ошибка
 *     2 — Сеть (AP) не найдена ................ WIFI_REASON_NO_AP_FOUND
 *     3 — Ошибка аутентификации (неверный пароль) WIFI_REASON_AUTH_FAIL
 *     4 — Таймаут 4-way handshake ............. WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
 *     5 — Ошибка ассоциации ................... WIFI_REASON_ASSOC_FAIL
 *
 * Индикация выполняется в отдельной задаче и не блокирует остальные.
 */

#include <stdio.h>
#include <string.h>

/* FreeRTOS — строго первым */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

/* Остальные заголовки ESP-IDF */
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "example";

#define BLINK_GPIO CONFIG_BLINK_GPIO
#define BLINK_GPIO_2 CONFIG_BLINK_GPIO_2

/* Биты группы событий Wi-Fi */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static uint8_t s_led_state = 0;
static volatile bool s_error_blink_active = false;
static TaskHandle_t s_err_blink_task = NULL;

/* ==================================================================
 *  Индикация ошибок Wi-Fi (не блокирует другие задачи)
 * ================================================================== */

/**
 * @brief Запустить индикацию ошибки Wi-Fi — мигание N раз.
 *
 * Функция НЕ блокирует вызывающий код: она лишь отправляет уведомление
 * задаче `err_blink_task`, которая выполнит мигание в фоне.
 *
 * Соответствие количества миганий типу ошибки:
 *   1 — неизвестная ошибка
 *   2 — сеть (AP) не найдена
 *   3 — ошибка аутентификации (неверный пароль)
 *   4 — таймаут 4-way handshake (неверный пароль/шифрование)
 *   5 — ошибка ассоциации
 *
 * @param blink_count число миганий (0 — ничего не делать)
 */
static void report_wifi_error(int blink_count)
{
    if (s_err_blink_task != NULL && blink_count > 0)
    {
        /* Перезаписываем предыдущее значение, если новая ошибка пришла раньше */
        xTaskNotify(s_err_blink_task, (uint32_t)blink_count, eSetValueWithOverwrite);
    }
}

/**
 * @brief Задача фоновой индикации ошибок внешним светодиодом.
 *
 * Ждёт уведомления бесконечно (без нагрузки на CPU). По получении
 * мигает `BLINK_GPIO_2` 0.3 с вкл / 0.3 с выкл нужное число раз,
 * затем держит паузу 2 с. На время индикации взводит флаг
 * `s_error_blink_active`, чтобы основной цикл не конфликтовал за пин.
 */
static void err_blink_task(void *arg)
{
    uint32_t count;
    while (1)
    {
        if (xTaskNotifyWait(0, 0xFFFFFFFF, &count, portMAX_DELAY) == pdTRUE)
        {
            s_error_blink_active = true;
            for (uint32_t i = 0; i < count; i++)
            {
                gpio_set_level(BLINK_GPIO_2, 1);
                vTaskDelay(pdMS_TO_TICKS(300));
                gpio_set_level(BLINK_GPIO_2, 0);
                vTaskDelay(pdMS_TO_TICKS(300));
            }
            /* Пауза 2 секунды после серии миганий */
            vTaskDelay(pdMS_TO_TICKS(2000));

            /* Возвращаем пин основному циклу в согласованное состояние */
            gpio_set_level(BLINK_GPIO_2, !s_led_state);
            s_error_blink_active = false;
        }
    }
}

/* ==================================================================
 *  GPIO и основной цикл
 * ================================================================== */

static void configure_led(void)
{
    ESP_LOGI(TAG, "Configuring GPIO LEDs");
    gpio_reset_pin(BLINK_GPIO);
    gpio_reset_pin(BLINK_GPIO_2);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(BLINK_GPIO_2, GPIO_MODE_OUTPUT);
}

static void blink_led(void)
{
    /* Пока идёт индикация ошибки, основной цикл не трогает BLINK_GPIO_2 */
    if (s_error_blink_active)
        return;

    gpio_set_level(BLINK_GPIO, s_led_state);
    gpio_set_level(BLINK_GPIO_2, !s_led_state);
}

/* ==================================================================
 *  Wi-Fi STA
 * ================================================================== */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "WiFi disconnected, reason=%d", ev->reason);

        /* Индикация ошибки — количество миганий зависит от причины */
        switch (ev->reason)
        {
        case WIFI_REASON_NO_AP_FOUND:
            report_wifi_error(2); /* AP не найдена */
            break;
        case WIFI_REASON_AUTH_FAIL:
            report_wifi_error(3); /* Неверный пароль */
            break;
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
            report_wifi_error(4); /* Ошибка 4-way handshake */
            break;
        case WIFI_REASON_ASSOC_FAIL:
            report_wifi_error(5); /* Ошибка ассоциации */
            break;
        default:
            report_wifi_error(1); /* Прочее */
            break;
        }

        /* При неверном пароле повторять бессмысленно */
        if (ev->reason == WIFI_REASON_AUTH_FAIL ||
            ev->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT)
        {
            ESP_LOGE(TAG, "Authentication failed. Check SSID/password.");
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            return;
        }

        if (s_retry_num < 5)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry WiFi connection (%d/5)", s_retry_num);
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();


    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // Инициализация NVS — обязательна для Wi-Fi
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_NAME,
            .password = CONFIG_WIFI_PASSWORD,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* Здесь НЕ используем ESP_ERROR_CHECK: неверный пароль — это
       штатная ошибка, а не повод для перезагрузки. */
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        report_wifi_error(1);
        return;
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi_init_sta: connecting to SSID '%s'", CONFIG_WIFI_NAME);

    /* Необязательное ожидание: убеждаемся в исходе, но не блокируем
       остальную систему — индикация ошибок уже работает в своей задаче. */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "Connected to %s", CONFIG_WIFI_NAME);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to connect to %s", CONFIG_WIFI_NAME);
    }
}

/* ==================================================================
 *  Точка входа
 * ================================================================== */

void app_main(void)
{
    configure_led();

    /* Задачу индикации запускаем ДО Wi-Fi, чтобы она была готова
       принимать уведомления об ошибках. */
    xTaskCreate(err_blink_task, "err_blink", 2048, NULL, 5, &s_err_blink_task);

    wifi_init_sta();

    while (1)
    {
        ESP_LOGI(TAG, "Turning LED %s", s_led_state ? "Blue" : "Red");
        blink_led();
        s_led_state = !s_led_state;
        vTaskDelay(CONFIG_BLINK_PERIOD / portTICK_PERIOD_MS);
    }
}