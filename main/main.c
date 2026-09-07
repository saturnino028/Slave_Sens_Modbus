#define CONFIG_I2C_SUPPRESS_DEPRECATE_WARN 1

#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "driver/uart.h"
#include "mbcontroller.h"

#define MB_PORT_NUM     UART_NUM_1
#define MB_SLAVE_ADDR   1
#define MB_TX_PIN       41
#define MB_RX_PIN       42
#define MB_RTS_PIN      45

#define I2C_SDA_PIN     47
#define I2C_SCL_PIN     48
#define I2C_PORT        I2C_NUM_0
#define AHT20_ADDR      0x38

#define QUEUE_SIZE      10

static const char *TAG = "MODBUS_SLAVE";

#pragma pack(push, 1)
typedef struct {
    int16_t temperature; 
    int16_t humidity;    
} sensor_data_t;
#pragma pack(pop)

static sensor_data_t holding_reg_area = {0};
static QueueHandle_t sensor_queue = NULL;

static void *slave_handle = NULL;

static void i2c_master_init(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
        .clk_flags = 0
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0));
}

static void modbus_slave_init(void) {
    // Configuração Modbus v2.x utilizando a estrutura ser_opts
    mb_communication_info_t comm_info = {
        .ser_opts.port = MB_PORT_NUM,
        .ser_opts.mode = MB_RTU,
        .ser_opts.baudrate = 9600,
        .ser_opts.parity = UART_PARITY_DISABLE,
        .ser_opts.uid = MB_SLAVE_ADDR,
        .ser_opts.data_bits = UART_DATA_8_BITS,
        .ser_opts.stop_bits = UART_STOP_BITS_1
    };

    // Cria a interface serial e retorna o manipulador (slave_handle)
    ESP_ERROR_CHECK(mbc_slave_create_serial(&comm_info, &slave_handle));

    // Mapeia os registradores passando o contexto
    mb_register_area_descriptor_t reg_area = {
        .type = MB_PARAM_HOLDING,
        .start_offset = 0,
        .address = (void*)&holding_reg_area,
        .size = sizeof(holding_reg_area)
    };
    ESP_ERROR_CHECK(mbc_slave_set_descriptor(slave_handle, reg_area));

    // Inicia a máquina de estados
    ESP_ERROR_CHECK(mbc_slave_start(slave_handle));

    // Configura hardware da UART para RS485 após o início da serial
    ESP_ERROR_CHECK(uart_set_pin(MB_PORT_NUM, MB_TX_PIN, MB_RX_PIN, MB_RTS_PIN, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_mode(MB_PORT_NUM, UART_MODE_RS485_HALF_DUPLEX));
    
    ESP_LOGI(TAG, "Modbus Slave RTU v2.1 iniciado no endereco %d", MB_SLAVE_ADDR);
}

static void aht20_task(void *pvParameters) {
    sensor_data_t new_data;
    uint8_t cmd[3] = {0xAC, 0x33, 0x00};
    uint8_t data[6];
    uint8_t print_counter = 0; // Inicia o contador de ciclos

    vTaskDelay(pdMS_TO_TICKS(100));

    while (1) {
        esp_err_t ret = i2c_master_write_to_device(I2C_PORT, AHT20_ADDR, cmd, sizeof(cmd), pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Falha ao enviar comando ao AHT20");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(80));

        ret = i2c_master_read_from_device(I2C_PORT, AHT20_ADDR, data, sizeof(data), pdMS_TO_TICKS(1000));
        if (ret == ESP_OK) {
            uint32_t raw_hum = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
            uint32_t raw_temp = (((uint32_t)(data[3] & 0x0F)) << 16) | ((uint32_t)data[4] << 8) | data[5];
            
            float hum = ((float)raw_hum / 1048576.0f) * 100.0f;
            float temp = ((float)raw_temp / 1048576.0f) * 200.0f - 50.0f;

            new_data.temperature = (int16_t)(temp * 10.0f);
            new_data.humidity = (int16_t)(hum * 10.0f);

            if (xQueueSend(sensor_queue, &new_data, 0) != pdTRUE) {
                sensor_data_t dummy;
                xQueueReceive(sensor_queue, &dummy, 0);
                xQueueSend(sensor_queue, &new_data, 0);
            }

            // Exibe os valores reais a cada 12 medições (60 segundos)
            if (++print_counter >= 12) {
                ESP_LOGI(TAG, "Leitura AHT20 [1 min] -> Temp: %.1f C | Umi: %.1f %%", temp, hum);
                print_counter = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500)); // Lê a cada 500 ms
    }
}

static void modbus_event_task(void *pvParameters) {
    sensor_data_t next_data;
    
    if (xQueueReceive(sensor_queue, &next_data, portMAX_DELAY) == pdTRUE) {
        holding_reg_area.temperature = next_data.temperature;
        holding_reg_area.humidity = next_data.humidity;
    }

    while (1) {
        // Na v2.x a função de checagem recebe o slave_handle 
        mb_event_group_t event = mbc_slave_check_event(slave_handle, MB_EVENT_HOLDING_REG_RD);
        
        if (event & MB_EVENT_HOLDING_REG_RD) {
            if (xQueueReceive(sensor_queue, &next_data, 0) == pdTRUE) {
                holding_reg_area.temperature = next_data.temperature;
                holding_reg_area.humidity = next_data.humidity;
                ESP_LOGI(TAG, "Lido pelo Mestre. Prox T: %d, U: %d", next_data.temperature, next_data.humidity);
            }
        }
    }
}

void app_main(void) {
    sensor_queue = xQueueCreate(QUEUE_SIZE, sizeof(sensor_data_t));

    i2c_master_init();
    modbus_slave_init();

    xTaskCreate(aht20_task, "aht20_task", 4096, NULL, 5, NULL);
    xTaskCreate(modbus_event_task, "modbus_event_task", 4096, NULL, 6, NULL);
}