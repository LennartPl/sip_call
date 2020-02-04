/*
   Copyright Christian Taedcke <hacking@taedcke.com>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */

#include "asio.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "driver/gpio.h"

extern "C" {
#include "esp_netif.h"
#include "esp_wifi_default.h"
}

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "sip_client/lwip_udp_client.h"
#include "sip_client/mbedtls_md5.h"
#include "sip_client/sip_client.h"

#include "button_handler.h"
#include "actuator_handler.h"

#include <string.h>

static constexpr auto BELL_GPIO_PIN = static_cast<gpio_num_t>(CONFIG_BELL_INPUT_GPIO);
static constexpr auto RING_DURATION_TIMEOUT_MSEC = CONFIG_RING_DURATION;

static constexpr auto ACTUATOR_GPIO_PIN = static_cast<gpio_num_t>(CONFIG_ACTUATOR_OUTPUT_GPIO);
static constexpr auto ACTUATOR_DURATION_TIMEOUT_MSEC = CONFIG_ACTUATOR_SWITCHING_DURATION;
static constexpr auto ACTUATOR_PHONE_BUTTON = CONFIG_ACTUATOR_PHONE_BUTTON;

#if CONFIG_ACTUATOR_ACTIVE_HIGH
static constexpr bool ACTUATOR_ACTIVE_HIGH = false;
#elif
static constexpr bool ACTUATOR_ACTIVE_HIGH = true;
#endif /*CONFIG_ACTUATOR_ACTIVE_HIGH*/

#if CONFIG_POWER_SAVE_MODEM_MAX
#define DEFAULT_PS_MODE WIFI_PS_MAX_MODEM
#elif CONFIG_POWER_SAVE_MODEM_MIN
#define DEFAULT_PS_MODE WIFI_PS_MIN_MODEM
#elif CONFIG_POWER_SAVE_NONE
#define DEFAULT_PS_MODE WIFI_PS_NONE
#else
#define DEFAULT_PS_MODE WIFI_PS_NONE
#endif /*CONFIG_POWER_SAVE_MODEM*/


/* FreeRTOS event group to signal when we are connected properly */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about one event
 * - are we connected to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

static const char* TAG = "main";

using SipClientT = SipClient<AsioUdpClient, MbedtlsMd5>;

static std::string ip_to_string(const esp_ip4_addr_t* ip)
{
    static constexpr size_t BUFFER_SIZE = 16;
    char buffer[BUFFER_SIZE];
    snprintf(buffer, BUFFER_SIZE, IPSTR, IP2STR(ip));
    return std::string(buffer);
}

#ifdef CONFIG_SIP_SERVER_IS_DHCP_SERVER
static std::string get_gw_ip_address(const system_event_sta_got_ip_t* got_ip)
{
    const ip4_addr_t* gateway = &got_ip->ip_info.gw;
    return ip_to_string(gateway);
}
#endif //CONFIG_SIP_SERVER_IS_DHCP_SERVER

static std::string get_local_ip_address(const esp_ip4_addr_t* got_ip)
{
    return ip_to_string(got_ip);
}

static void event_handler(void* arg, esp_event_base_t event_base,
    int32_t event_id, void* event_data)
{
    SipClientT* client = static_cast<SipClientT*>(arg);

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        client->deinit();
        /* This is a workaround as ESP32 WiFi libs don't currently auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        esp_ip4_addr_t* got_ip = &event->ip_info.ip;
#ifdef CONFIG_SIP_SERVER_IS_DHCP_SERVER
        client->set_server_ip(get_gw_ip_address(got_ip));
#endif //CONFIG_SIP_SERVER_IS_DHCP_SERVER
        client->set_my_ip(get_local_ip_address(got_ip));
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
    }
}

static void initialize_wifi()
{
    ESP_ERROR_CHECK(esp_netif_init());
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, CONFIG_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, CONFIG_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.bssid_set = false;

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "esp_wifi_set_ps().");
    esp_wifi_set_ps(DEFAULT_PS_MODE);
}

struct handlers_t
{
    SipClientT& client;
    ButtonInputHandler<SipClientT, BELL_GPIO_PIN, RING_DURATION_TIMEOUT_MSEC>& button_input_handler;
	ActuatorHandler<ACTUATOR_GPIO_PIN, false, ACTUATOR_DURATION_TIMEOUT_MSEC>& actuator_handler;
    asio::io_context& io_context;
};

static void sip_task(void* pvParameters)
{
    handlers_t* handlers = static_cast<handlers_t*>(pvParameters);
    SipClientT& client = handlers->client;
    ButtonInputHandler<SipClientT, BELL_GPIO_PIN, RING_DURATION_TIMEOUT_MSEC>& button_input_handler = handlers->button_input_handler;
	ActuatorHandler<ACTUATOR_GPIO_PIN, ACTUATOR_ACTIVE_HIGH, ACTUATOR_DURATION_TIMEOUT_MSEC>& actuator_handler = handlers->actuator_handler;

    for (;;)
    {
        // Wait for wifi connection
        xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);

        if (!client.is_initialized())
        {
            bool result = client.init();
            ESP_LOGI(TAG, "SIP client initialized %ssuccessfully", result ? "" : "un");
            if (!result)
            {
                ESP_LOGI(TAG, "Waiting to try again...");
                vTaskDelay(2000 / portTICK_RATE_MS);
                continue;
            }
            client.set_event_handler([&button_input_handler, &actuator_handler](const SipClientEvent& event) {
                switch (event.event)
                {
                case SipClientEvent::Event::CALL_START:
                    ESP_LOGI(TAG, "Call start");
                    break;
                case SipClientEvent::Event::CALL_CANCELLED:
                    ESP_LOGI(TAG, "Call cancelled, reason %d", (int)event.cancel_reason);
                    button_input_handler.call_end();
                    break;
                case SipClientEvent::Event::CALL_END:
                    ESP_LOGI(TAG, "Call end");
                    button_input_handler.call_end();
                    break;
                case SipClientEvent::Event::BUTTON_PRESS:
                    ESP_LOGI(TAG, "Got button press: %c for %d milliseconds", event.button_signal, event.button_duration);
                    
                    if(event.button_signal == ACTUATOR_PHONE_BUTTON[0])
                        actuator_handler.trigger();
                    
					//###############
					/*
#define GPIO GPIO_NUM_13					
					gpio_config_t io_conf;
					//disable interrupt
					io_conf.intr_type = GPIO_INTR_DISABLE;
					//set as output mode
					io_conf.mode = GPIO_MODE_OUTPUT;
					//bit mask of the pins that you want to set,e.g.GPIO18/19
					io_conf.pin_bit_mask = (1ULL << GPIO);;
					//disable pull-down mode
					io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
					//disable pull-up mode
					io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
					//configure GPIO with the given settings
					gpio_config(&io_conf);
					
					gpio_set_level(GPIO, 0);
					vTaskDelay(2000 / portTICK_RATE_MS);
					gpio_set_level(GPIO, 1);
					*/
					//########################
                    break;
                }
            });
        }

        handlers->io_context.run();
    }
}

extern "C" void app_main(void)
{
    // seed for std::rand() used in the sip client
    std::srand(esp_random());
    nvs_flash_init();
    initialize_wifi();

    // reseed after initializing wifi
    std::srand(esp_random());

    // Execute io_context.run() only from one thread
    asio::io_context io_context { 1 };

    SipClientT client { io_context, CONFIG_SIP_USER, CONFIG_SIP_PASSWORD, CONFIG_SIP_SERVER_IP, CONFIG_SIP_SERVER_PORT, CONFIG_LOCAL_IP };
    ButtonInputHandler<SipClientT, BELL_GPIO_PIN, RING_DURATION_TIMEOUT_MSEC> button_input_handler(client);
	ActuatorHandler<ACTUATOR_GPIO_PIN, ACTUATOR_ACTIVE_HIGH, ACTUATOR_DURATION_TIMEOUT_MSEC> actuator_handler;

    handlers_t handlers { client, button_input_handler, actuator_handler, io_context };

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, &client));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, &client));

    // without pinning this to core 0, funny stuff (crashes) happen,
    // because some c++ objects are not fully initialized
    xTaskCreatePinnedToCore(&sip_task, "sip_task", 8192, &handlers, 5, NULL, 0);

    //blocks forever
    button_input_handler.run();
}
