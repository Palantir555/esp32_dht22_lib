## Usage Notes

API:

1. Call `dht_init()` with the GPIO pin connected to the DHT22
2. Call `dht_read()` with the address of a pre-allocated `dht_reading` struct

Read errors:

Given the time sensitivity of bit banging, interrupts during the read process
can cause errors. Those errors will be caught by the checksum validation,
and dht_read will return `DHT_ERR_BADCHECKSUM`. Make sure your logic handles
read errors gracefully.

Also, in order to minimize the number of read errors, you should pin the sensor
reading task to whichever processor core handles the fewest interrupts. By
default, that's core 1. More details in `dht_sensor.c` 

## Example

```c
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dht_sensor.h"

#define DHT_SENSOR_PIN (22)
#define DHT_SENSOR_READ_DELAY_MS (2000)
#define DHT_TASK_CORE (1)

dht_sensor sensor = DHT_SENSOR_PIN;

static void sensor_task(void* pvparameters)
{
    dht_reading sensor_reading = {
        .temperature = 0., .humidity = 0., .checksum = 0x00};
    if(dht_init(sensor) != DHT_OK)
    {
        printf("Error: DHT sensor initialization failed\n");
        while(true);
    }

    while(true)
    {
        if(dht_read(sensor, &sensor_reading) == DHT_OK)
        {
            printf("- T: %02.2f\n", sensor_reading.temperature);
            printf("- H: %02.2f\n", sensor_reading.humidity);
        }
        else
        {
            printf("dht_read failed\n");
        }
        vTaskDelay(DHT_SENSOR_READ_DELAY_MS / portTICK_RATE_MS);
    }
}

void app_main(void)
{
    xTaskCreatePinnedToCore(&sensor_task, "sensor_task", 2048, NULL,
                            5, NULL, DHT_TASK_CORE);
}
```

## License

This project is [Unlicensed](https://unlicense.org/). Use it however you want.
