/*
 * Copyright (c) 2020, Carlo Vallati, University of Pisa
  * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*---------------------------------------------------------------------------*/
#include "contiki.h"
#include "net/routing/routing.h"
#include "mqtt.h"
#include "dev/rgb-led/rgb-led.h"
#include "net/ipv6/uip.h"
#include "net/ipv6/uip-icmp6.h"
#include "net/ipv6/sicslowpan.h"
#include "sys/etimer.h"
#include "sys/ctimer.h"
#include "lib/sensors.h"
#include "dev/button-hal.h"
#include "os/sys/log.h"
#include "mqtt-client.h"

#include <time.h>
#include <string.h>
#include <strings.h>
/*---------------------------------------------------------------------------*/
#define LOG_MODULE "mqtt-client-food"
#ifdef MQTT_CLIENT_CONF_LOG_LEVEL
#define LOG_LEVEL MQTT_CLIENT_CONF_LOG_LEVEL
#else
#define LOG_LEVEL LOG_LEVEL_DBG
#endif

/*---------------------------------------------------------------------------*/
/* MQTT broker address. */
#define MQTT_CLIENT_BROKER_IP_ADDR "fd00::1"

static const char *broker_ip = MQTT_CLIENT_BROKER_IP_ADDR;

// Default config values
#define DEFAULT_BROKER_PORT         1883
#define DEFAULT_PUBLISH_INTERVAL    (5 * CLOCK_SECOND)
#define EATING_RATE    (14 * CLOCK_SECOND)




/*---------------------------------------------------------------------------*/
/* Various states */
static uint8_t state;

#define STATE_INIT              0
#define STATE_NET_OK          1
#define STATE_CONNECTING      2
#define STATE_CONNECTED       3
#define STATE_SUBSCRIBED      4
#define STATE_DISCONNECTED    5

static uint8_t boot;
#define BOOT_NOT_STARTED      0
#define BOOT_INIT          1
#define BOOT_ID_NEGOTIATION   2
#define BOOT_ID_DENIED        3
#define BOOT_COMPLETED        4
#define BOOT_FAILED           5
/*---------------------------------------------------------------------------*/
/* PUBLISH/SUBSCRIBE MESSAGE TEMPLATES */
#define NODE_TYPE "food"
#define TOPIC_ID_CONFIG "id_config"
#define TOPIC_SENSOR_DATA "food_sensor"
#define PUBLISH_MSG_TEMPLATE "bowl:%d;food-level:%d"
#define TOPIC_ACTUATOR "food_actuator"
/*---------------------------------------------------------------------------*/
PROCESS_NAME(mqtt_client_process);
AUTOSTART_PROCESSES(&mqtt_client_process);

/*---------------------------------------------------------------------------*/
/* Maximum TCP segment size for outgoing segments of our socket */
#define MAX_TCP_SEGMENT_SIZE    32
#define CONFIG_IP_ADDR_STR_LEN   64
/*---------------------------------------------------------------------------*/
/*
 * Buffers for Client ID and Topics.
 * Make sure they are large enough to hold the entire respective string
 */
#define BUFFER_SIZE 64

static char client_id[BUFFER_SIZE];
static char pub_topic[BUFFER_SIZE];
static char sub_topic[BUFFER_SIZE];

// Periodic timer to check the state of the MQTT client
#define STATE_MACHINE_PERIODIC     (CLOCK_SECOND >> 1)
static struct etimer periodic_timer;
static struct etimer meal_timer; //Pet behaviour simulation timer

/*---------------------------------------------------------------------------*/
/*
 * The main MQTT buffers.
 * We will need to increase if we start publishing more data.
 */
#define APP_BUFFER_SIZE 512
static char app_buffer[APP_BUFFER_SIZE];
/*---------------------------------------------------------------------------*/
static struct mqtt_message *msg_ptr = 0;

static struct mqtt_connection conn;

mqtt_status_t status;
char broker_address[CONFIG_IP_ADDR_STR_LEN];

/*---------------------------------------------------------------------------*/
PROCESS(mqtt_client_process,
"MQTT Client food sensor");

static int foodLevel = 50; //weight of food inside the container
static bool filling = false; // actuator status detected on the container
static bool eating = false; //Pet behaviour simulation status
unsigned short containerID = 0;
static int meal_size = 30;
static int meal_charge = 100;

/*---------------------------------------------------------------------------*/
static void pub_handler(const char *topic, uint16_t topic_len, const uint8_t *chunk, uint16_t chunk_len) {
    printf("Pub Handler: topic='%s' (len=%u), chunk_len=%u\n", topic, topic_len, chunk_len);
    char msg_template[128];
    if (strcmp(topic, TOPIC_ID_CONFIG) == 0) {
        // received answer during Id negotiation
        snprintf(msg_template, sizeof(msg_template), "%s %d approved", NODE_TYPE, containerID);
        if (strcmp((const char *) chunk, msg_template) == 0) {
            // controlled accepted Id proposal
            mqtt_unsubscribe(&conn, NULL, TOPIC_ID_CONFIG);
            strcpy(sub_topic, TOPIC_ACTUATOR);
            mqtt_subscribe(&conn, NULL, sub_topic, MQTT_QOS_LEVEL_0);
            state = STATE_SUBSCRIBED;
        } else {
            snprintf(msg_template, sizeof(msg_template), "%s %d denied", NODE_TYPE, containerID);
            if (strcmp((const char *) chunk, msg_template) == 0) { // controlled rejected Id proposal
                boot = BOOT_ID_DENIED;
            }
        }
    }
        // received message for actuator
    else if (strcmp(topic, TOPIC_ACTUATOR) == 0) {
        snprintf(msg_template, sizeof(msg_template), "%d start", containerID);
        if (strcmp((const char *) chunk, msg_template) == 0) {
            boot = BOOT_COMPLETED;
            printf("Food Sensor %d Started \n", containerID);
            LOG_INFO("Starting sensor %d \n", containerID);
            rgb_led_set(RGB_LED_GREEN);
        } else {
            snprintf(msg_template, sizeof(msg_template), "%d filling", containerID);
            if (strcmp((const char *) chunk, msg_template) == 0) {
                LOG_INFO("Bowl %d refilling started \n", containerID);
                printf("Bowl %d refilling started \n", containerID);
                filling = true;
            } else {
                snprintf(msg_template, sizeof(msg_template), "%d stop", containerID);
                if (strcmp((const char *) chunk, msg_template) == 0) {
                    LOG_INFO("Bowl %d  refilling stopped \n", containerID);
                    filling = false;
                }
            }
        }
    }
}

/*---------------------------------------------------------------------------*/
static void mqtt_event(struct mqtt_connection *m, mqtt_event_t event, void *data) {
    switch (event) {
        case MQTT_EVENT_CONNECTED: {
            printf("Application has a MQTT connection\n");
            state = STATE_CONNECTED;
            break;
        }
        case MQTT_EVENT_DISCONNECTED: {
            printf("MQTT Disconnect. Reason %u\n", *((mqtt_event_t *) data));
            state = STATE_DISCONNECTED;
            process_poll(&mqtt_client_process);
            break;
        }
        case MQTT_EVENT_PUBLISH: {
            msg_ptr = data;
            pub_handler(msg_ptr->topic, strlen(msg_ptr->topic), msg_ptr->payload_chunk, msg_ptr->payload_length);
            break;
        }
        case MQTT_EVENT_SUBACK: {
#if MQTT_311
            mqtt_suback_event_t *suback_event = (mqtt_suback_event_t *)data;
            if(suback_event->success) {
              printf("Application is subscribed to topic successfully\n");
            } else {
              printf("Application failed to subscribe to topic (ret code %x)\n", suback_event->return_code);
            }
#else
            printf("Application is subscribed to topic successfully\n");
#endif
            break;
        }
        case MQTT_EVENT_UNSUBACK: {
            printf("Application is unsubscribed to topic successfully\n");
            break;
        }
        case MQTT_EVENT_PUBACK: {
            printf("Publishing complete.\n");
            break;
        }
        default:
            printf("Application got a unhandled MQTT event: %i\n", event);
            break;
    }
}

static bool have_connectivity(void) {
    if (uip_ds6_get_global(ADDR_PREFERRED) == NULL ||
        uip_ds6_defrt_choose() == NULL) {
        return false;
    }
    return true;
}

void meal_time(int *foodLevel) {
    *foodLevel -= meal_size;
    if (*foodLevel < 0)
        *foodLevel = 0;
}

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(mqtt_client_process, ev, data) {
    boot = BOOT_NOT_STARTED;
    PROCESS_BEGIN();


    printf("MQTT Client Process\n");

    // Initialize the ClientID as MAC address
    snprintf(client_id, BUFFER_SIZE, "%02x%02x%02x%02x%02x%02x",
             linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1],
             linkaddr_node_addr.u8[2], linkaddr_node_addr.u8[5],
             linkaddr_node_addr.u8[6], linkaddr_node_addr.u8[7]);

    // Broker registration
    mqtt_register(&conn, &mqtt_client_process, client_id, mqtt_event,
                  MAX_TCP_SEGMENT_SIZE);
    state = STATE_INIT;

    // Initialize periodic timer to check the status
    etimer_set(&periodic_timer, DEFAULT_PUBLISH_INTERVAL);
    etimer_set(&meal_timer, EATING_RATE);
    rgb_led_set(RGB_LED_RED);

    boot = BOOT_INIT;
    /* Main loop */
    while (1) {

        PROCESS_YIELD();
        if (etimer_expired(&meal_timer) {
            meal_time(*foodLevel);
            etimer_reset(&meal_timer);
        }

        if ((ev == PROCESS_EVENT_TIMER && data == &periodic_timer) || ev == PROCESS_EVENT_POLL) {

            if (state == STATE_INIT && have_connectivity()) {
                state = STATE_NET_OK;
            }

            if (state == STATE_NET_OK) {
    // Connect to MQTT server
                printf("Connecting!\n");
                memcpy(broker_address, broker_ip, strlen(broker_ip));
                mqtt_connect(&conn, broker_address, DEFAULT_BROKER_PORT, (DEFAULT_PUBLISH_INTERVAL * 3) / CLOCK_SECOND,
                             MQTT_CLEAN_SESSION_ON);
                state = STATE_CONNECTING;
            }

            if (state == STATE_CONNECTED) {
    // Subscribe to a topic
                strcpy(sub_topic, TOPIC_ID_CONFIG);
                status = mqtt_subscribe(&conn, NULL, sub_topic, MQTT_QOS_LEVEL_0);
                printf("Subscribing!\n");
                if (status == MQTT_STATUS_OUT_QUEUE_FULL) {
                    LOG_ERR("Tried to subscribe but command queue was full!\n");
                    PROCESS_EXIT();
                }
                containerID = 1 + (int) random_rand() % 100;
                boot = BOOT_ID_NEGOTIATION;
            }


            if ((boot = BOOT_COMPLETED) && (state == STATE_SUBSCRIBED) {
    // Publish something
                sprintf(pub_topic, "%s", TOPIC_SENSOR_DATA);

                if (filling) {
                    foodLevel += meal_charge;
                }
                LOG_INFO("New values: %d\n", foodLevel);
                rgb_led_set(RGB_LED_GREEN);
                sprintf(app_buffer, PUBLISH_MSG_TEMPLATE, containerID, foodLevel);

                mqtt_publish(&conn, NULL, pub_topic, (uint8_t *) app_buffer, strlen(app_buffer), MQTT_QOS_LEVEL_0,
                             MQTT_RETAIN_OFF);
            }
            if (boot == BOOT_ID_DENIED) {
                containerID = 1 + (int) random_rand() % 100;
                boot = BOOT_ID_NEGOTIATION;
            }
            if (boot == BOOT_ID_NEGOTIATION) {
    // id negotiation
                sprintf(app_buffer, NODE_TYPE + " " + containerID + " awakens");
                mqtt_publish(&conn, NULL, TOPIC_ID_CONFIG, (uint8_t *) app_buffer, strlen(app_buffer), MQTT_QOS_LEVEL_0,
                             MQTT_RETAIN_OFF);
            }

            if (state == STATE_DISCONNECTED) {
                LOG_ERR("Disconnected from MQTT broker\n");
                boot = BOOT_FAILED;
                rgb_led_set(RGB_LED_RED);
    // Recover from error
                state = STATE_INIT;
            }
            etimer_set(&periodic_timer, DEFAULT_PUBLISH_INTERVAL);
        }
    }
    PROCESS_END();
}
/*---------------------------------------------------------------------------*/