/* Given the custom 1-Wire protocol used by the DHT sensors, we need to bit-bang
 * its implementation. Bit-banging is -by nature- sensitive to timing issues.
 * I've found it to be particularly sensitive to WiFi and networking interrupts.
 * There's 2 ways to minimize the number of tiemeouts found when querying a DHT:
 *      1. [Not recommended] Disable interrupts for the duration of dht_read().
 *         Should remove most -if not all- timeouts
 *      2. [Recommended] Always run dht_read() from a task pinned to the core 1.
 *         The ESP32 uses its CPU Core 0 by default, so all wifi/networking/etc
 *         interrupts (besides some exceptions) are handled from that core.
 *         By pinning the DHT-reading task to Core 1 with
 *         xTaskCreatePinnedToCore, we can eliminate most timeouts; but not all
 * Besides those attenuations of the issue, make sure your application logic
 * can handle DHT timeout errors gracefully.
 */

#include <stdbool.h>
#include <string.h>

#include "dht_sensor.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "rom/ets_sys.h"

#define DHT_BYTES_PER_READ (5) /* 40 bits per transmission */

/* Data line timings defined by the DHT22 spec, in microseconds */
const int spec_us_out_request_low      = 3000; /* [1, 10]ms */
const int spec_us_out_request_high     = 20;   /* [20, 40]us */
const int spec_us_in_ready_signal_half = 80;   /* 80us */
const int spec_us_in_data_bit_low      = 50;   /* 50us */
const int spec_us_in_data_bit_high     = 70;   /* 70us */
const int spec_us_bit_length_threshold = 40;   /* [26, 28]us==0 | 70us==1 */
const int measured_us_max_transition_t = 10;

/* MCU pulls low, then up to request reading. The DHT then indicates the
 * transmission is starting by setting the line low and holding it for 80us,
 * then high again for another 80us */
static inline dht_retval dht_request_readings(dht_sensor dht)
{
    /* Set data pin as output to request data from DHT22 */
    if(gpio_set_direction(dht, GPIO_MODE_OUTPUT) != ESP_OK)
    {
        return DHT_ERR_GPIO;
    }

    if(gpio_set_level(dht, 0) != ESP_OK)
    {
        return DHT_ERR_GPIO;
    }
    ets_delay_us(spec_us_out_request_low);

    if(gpio_set_level(dht, 1) != ESP_OK)
    {
        return DHT_ERR_GPIO;
    }
    ets_delay_us(spec_us_out_request_high);
    /* Set pin as input to wait for data */
    if(gpio_set_direction(dht, GPIO_MODE_INPUT) != ESP_OK)
    {
        return DHT_ERR_GPIO;
    }
    return DHT_OK;
}

/* After data has been requested, the DHT indicates the transmission start by
 * holding the line low for 80us, then up for 80us */
static inline dht_retval dht_await_data(dht_sensor dht)
{
    const int timeout_usec = spec_us_in_ready_signal_half + 10;
    int64_t   uptime_entry = esp_timer_get_time();
    while(gpio_get_level(dht) == 0)
    {
        if((esp_timer_get_time() - uptime_entry) >= timeout_usec)
        {
            return DHT_ERR_TIMEOUT;
        }
    }

    uptime_entry = esp_timer_get_time();
    while(gpio_get_level(dht) == 1)
    {
        if((esp_timer_get_time() - uptime_entry) >= timeout_usec)
        {
            return DHT_ERR_TIMEOUT;
        }
    }
    return DHT_OK;
}

/* Each data bit is low for 50us, then high for [26, 28]us == 0 || 70us == 1 */
static inline dht_retval dht_read_bit(dht_sensor dht, bool* read_bit)
{
    const int timeout_low_usec  = spec_us_in_data_bit_low + 20;
    const int timeout_high_usec = spec_us_in_data_bit_high + 20;
    int64_t   usec_spent_high   = 0;
    int64_t   uptime_entry      = 0;

    /* Bit is 1 or 0 based on the time spent at HIGH state. Wait for HIGH */
    uptime_entry = esp_timer_get_time();
    while(gpio_get_level(dht) == 0)
    {
        if((esp_timer_get_time() - uptime_entry) >= timeout_low_usec)
        {
            return DHT_ERR_TIMEOUT;
        }
    }

    uptime_entry = esp_timer_get_time();
    while(gpio_get_level(dht) == 1)
    {
        if((esp_timer_get_time() - uptime_entry) >= timeout_high_usec)
        {
            return DHT_ERR_TIMEOUT;
        }
    }
    usec_spent_high = esp_timer_get_time() - uptime_entry;

    (usec_spent_high < spec_us_bit_length_threshold) ? (*read_bit = 0)
                                                     : (*read_bit = 1);
    return DHT_OK;
}

dht_retval dht_init(dht_sensor dht)
{
    if(gpio_set_direction(dht, GPIO_MODE_OUTPUT) != ESP_OK)
    {
        return DHT_ERR_GPIO;
    }
    if(gpio_set_level(dht, 1) != ESP_OK)
    {
        return DHT_ERR_GPIO;
    }
    return DHT_OK;
}

/* Takes about 7.2ms to complete a transaction */
dht_retval dht_read(dht_sensor dht, dht_reading* reading)
{
    uint8_t buf[DHT_BYTES_PER_READ];
    uint8_t byte_in = 0x00;
    bool    bit_in  = false;

    memset(buf, 0x00, sizeof(buf));
    memset(reading, 0x00, sizeof(dht_reading));

    /* Request sensor reading */
    if(dht_request_readings(dht) != DHT_OK)
    {
        return DHT_ERR_GPIO;
    }

    /* Await transmission start signal */
    if(dht_await_data(dht) != DHT_OK)
    {
        return DHT_ERR_TIMEOUT;
    }

    /* Read sensor transmission */
    for(uint8_t byte_idx = 0; byte_idx < DHT_BYTES_PER_READ; byte_idx++)
    {
        byte_in = 0x00;
        for(int bit_idx = 7; bit_idx >= 0; bit_idx--)
        {
            if(dht_read_bit(dht, &bit_in) != DHT_OK)
            {
                return DHT_ERR_TIMEOUT;
            }
            if(bit_in == true)
            {
                byte_in |= (0x01 << bit_idx);
            }
            /* Provide some breathing room for level transistional period */
            ets_delay_us(measured_us_max_transition_t);
        }
        buf[byte_idx] = byte_in;
    }

    /* Validate data with checksum */
    if(buf[4] != ((buf[0] + buf[1] + buf[2] + buf[3]) & 0xFF))
    {
        return DHT_ERR_BADCHECKSUM;
    }

    /* Parse data buffer */
    reading->humidity = buf[0];
    reading->humidity *= 0x100;
    reading->humidity += buf[1];
    reading->humidity /= 10;

    reading->temperature = buf[2] & 0x7F;
    reading->temperature *= 0x100;
    reading->temperature += buf[3];
    reading->temperature /= 10;
    if(buf[2] & 0x80)
    {
        reading->temperature *= -1;
    }

    reading->checksum = buf[4];

    return DHT_OK;
}
