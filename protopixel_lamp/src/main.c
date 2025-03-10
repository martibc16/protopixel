#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "driver/ledc.h"

#include "esp_mac.h"

#include "espnow.h"
#include "espnow_ctrl.h"
#include "espnow_utils.h"

#include "iot_button.h"
#include "button_gpio.h"

//GPIO definitions
#define SWITCH_LAMP_GPIO     GPIO_NUM_32
#define LED_LAMP_GPIO        GPIO_NUM_33

// LEDC definitions
#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_OUTPUT_IO          (5) // Define the output GPIO
#define LEDC_CHANNEL            LEDC_CHANNEL_0
#define LEDC_DUTY_RES           LEDC_TIMER_13_BIT // Set duty resolution to 13 bits
#define LEDC_DUTY               (4096) // Set duty to 50%. (2 ** 13) * 50% = 4096
#define LEDC_FREQUENCY          (4000) // Frequency in Hertz. Set frequency at 4 kHz

static const char *TAG = "LAMP_app_main";
static int32_t led_level = 0;

typedef enum {
    APP_ESPNOW_CTRL_INIT,
    APP_ESPNOW_CTRL_BOUND,
    APP_ESPNOW_CTRL_MAX
} app_espnow_ctrl_status_t;

static app_espnow_ctrl_status_t s_espnow_ctrl_status = APP_ESPNOW_CTRL_INIT;

static char *bind_error_to_string(espnow_ctrl_bind_error_t bind_error)
{
    switch (bind_error) {
    case ESPNOW_BIND_ERROR_NONE: {
        return "No error";
        break;
    }

    case ESPNOW_BIND_ERROR_TIMEOUT: {
        return "bind timeout";
        break;
    }

    case ESPNOW_BIND_ERROR_RSSI: {
        return "bind packet RSSI below expected threshold";
        break;
    }

    case ESPNOW_BIND_ERROR_LIST_FULL: {
        return "bindlist is full";
        break;
    }

    default: {
        return "unknown error";
        break;
    }
    }
}

//Necessary for ESP-NOW protocol
static void app_wifi_init()
{
    esp_event_loop_create_default();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void app_led_init(void)
{
    // Prepare and then apply the LEDC PWM timer configuration
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_MODE,
        .timer_num        = LEDC_TIMER,
        .duty_resolution  = LEDC_DUTY_RES,
        .freq_hz          = LEDC_FREQUENCY,  // Set output frequency at 4 kHz
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Prepare and then apply the LEDC PWM channel configuration
    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_MODE,
        .channel        = LEDC_CHANNEL,
        .timer_sel      = LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = LED_LAMP_GPIO,
        .duty           = 0, // Set duty to 0%
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

void app_led_set_level(uint8_t brightness)
{
    //By changing the duty cycle we control the brightness
    //Duty resolution is 13 bits. So 100% duty cycle corresponds to 2^13=8192
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, (uint32_t)(brightness*8192/100));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));
}

//helper function to update the led level and send the data through ESP-NOW
static void app_initiator_send_data(){
    app_led_set_level(led_level);

    if (s_espnow_ctrl_status == APP_ESPNOW_CTRL_BOUND) {
        ESP_LOGD(TAG, "initiator send press");
        ESP_LOGI(TAG, "Send signal to lamp: %"PRIu32, led_level);
        espnow_ctrl_initiator_send(ESPNOW_ATTRIBUTE_KEY_1, ESPNOW_ATTRIBUTE_POWER, led_level);
    } else {
        ESP_LOGI(TAG, "please double click to bind the devices firstly");
    }
}

//The button has been pressed one time, this invokes this callback --> we toggle the lamp in our device and we send the status change to the switch
static void app_initiator_send_press_cb(void *arg, void *usr_data)
{
    ESP_ERROR_CHECK(!(BUTTON_SINGLE_CLICK == iot_button_get_event(arg)));
    if(led_level > 0)
        led_level = 0;
    else
        led_level=100;

    app_initiator_send_data();
}

//The button has been long-pressed, this invokes this callback --> we increase brightness in our lamp and send the new state
static void app_initiator_long_press_cb(void *arg, void *usr_data){
    static int direction = 1; 

    ESP_ERROR_CHECK(!(BUTTON_LONG_PRESS_HOLD == iot_button_get_event(arg)));

    led_level += (direction * 8);

    if (led_level > 100) {
        direction = -1;  // Start decreasing
        led_level = 100;
    } else if (led_level < 0) {
        direction = 1;   // Start increasing
        led_level = 0;
    }

    app_initiator_send_data();
}

//Double click triggers this function. This initiates ESP-NOW's pairing system
static void app_initiator_bind_press_cb(void *arg, void *usr_data)
{
    ESP_ERROR_CHECK(!(BUTTON_DOUBLE_CLICK == iot_button_get_event(arg)));

    if (s_espnow_ctrl_status == APP_ESPNOW_CTRL_INIT) {
        ESP_LOGI(TAG, "initiator bind press");
        espnow_ctrl_initiator_bind(ESPNOW_ATTRIBUTE_KEY_1, true);
        s_espnow_ctrl_status = APP_ESPNOW_CTRL_BOUND;
    } else {
        ESP_LOGI(TAG, "this device is already in bound status");
    }
}

static void button_init(void)
{
    app_led_init();

    button_config_t btn_cfg = {0};
    button_gpio_config_t  gpio_cfg  = {
        .gpio_num = SWITCH_LAMP_GPIO,
        .active_level = 0,
    };

    button_handle_t button_handle;
    iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &button_handle);

    iot_button_register_cb(button_handle, BUTTON_SINGLE_CLICK, NULL, app_initiator_send_press_cb, NULL);
    iot_button_register_cb(button_handle, BUTTON_DOUBLE_CLICK, NULL, app_initiator_bind_press_cb, NULL);
    iot_button_register_cb(button_handle, BUTTON_LONG_PRESS_HOLD, NULL, app_initiator_long_press_cb, NULL);
}

//The switch has been remotely pressed. This info is transmited via ESP-NOW and we recieve it here.
static void app_responder_ctrl_data_cb(espnow_attribute_t initiator_attribute,
                              espnow_attribute_t responder_attribute,
                              uint32_t status)
{
    ESP_LOGI(TAG, "app_responder_ctrl_data_cb, initiator_attribute: %d, responder_attribute: %d, value: %" PRIu32 "",
             initiator_attribute, responder_attribute, status);
    
    
    led_level=status;
    
    //Update the lamp's light with the new state
    app_led_set_level(led_level);

    //We now send the updated state of the lamp to the switch so that it can proceed with the necessary steps:
    app_initiator_send_data();
}

static void app_responder_init(void)
{
    ESP_ERROR_CHECK(espnow_ctrl_responder_bind(30 * 1000, -55, NULL));
    // Register callback for recieved data
    espnow_ctrl_responder_data(app_responder_ctrl_data_cb);
}

static void app_espnow_event_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    if (base != ESP_EVENT_ESPNOW) {
        return;
    }

    switch (id) {
    case ESP_EVENT_ESPNOW_CTRL_BIND: {
        espnow_ctrl_bind_info_t *info = (espnow_ctrl_bind_info_t *)event_data;
        ESP_LOGI(TAG, "bind, uuid: " MACSTR ", initiator_type: %d", MAC2STR(info->mac), info->initiator_attribute);
        break;
    }

    case ESP_EVENT_ESPNOW_CTRL_BIND_ERROR: {
        espnow_ctrl_bind_error_t *bind_error = (espnow_ctrl_bind_error_t *)event_data;
        ESP_LOGW(TAG, "bind error: %s", bind_error_to_string(*bind_error));
        break;
    }

    case ESP_EVENT_ESPNOW_CTRL_UNBIND: {
        espnow_ctrl_bind_info_t *info = (espnow_ctrl_bind_info_t *)event_data;
        ESP_LOGI(TAG, "unbind, uuid: " MACSTR ", initiator_type: %d", MAC2STR(info->mac), info->initiator_attribute);
        break;
    }

    default:
        break;
    }
}

void app_main(void)
{
    espnow_storage_init();

    app_wifi_init();
    button_init();

    espnow_config_t espnow_config = ESPNOW_INIT_CONFIG_DEFAULT();
    espnow_init(&espnow_config);

    esp_event_handler_register(ESP_EVENT_ESPNOW, ESP_EVENT_ANY_ID, app_espnow_event_handler, NULL);

    app_responder_init();
}