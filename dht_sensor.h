/* Author: Juan Carlos Jimenez Caballero */

#ifndef DHT_SENSOR_H_
#define DHT_SENSOR_H_

#include <stdint.h>

typedef int dht_sensor;

typedef struct
{
    float   temperature;
    float   humidity;
    uint8_t checksum; /* Here in case flash storage validation is needed */
} dht_reading;

typedef enum
{
    DHT_OK = 0,
    DHT_ERR_GPIO,
    DHT_ERR_BADCHECKSUM,
    DHT_ERR_TIMEOUT
} dht_retval;

dht_retval dht_init(dht_sensor dht);
dht_retval dht_read(dht_sensor dht, dht_reading* reading);

#endif /* DHT_SENSOR_H_ */
