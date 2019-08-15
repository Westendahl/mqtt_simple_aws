/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <stdio.h>
#include <uart.h>
#include <string.h>

#include <net/mqtt.h>
#include <net/socket.h>
#include <lte_lc.h>

#include <device.h>
#include <gpio.h>
#include <dk_buttons_and_leds.h>

#include <string.h>

#if defined(CONFIG_PROVISION_CERTIFICATES)
#if defined(CONFIG_BSD_LIBRARY)
#include "nrf_inbuilt_key.h"
#endif
#endif

#include "certificates.h"

#if defined(CONFIG_MQTT_LIB_TLS)
//#include CONFIG_CERTIFICATES_FILE
static sec_tag_t sec_tag_list[] = { CONFIG_SEC_TAG };
#endif

//#define KEY_ECHO_MASK  DK_BTN1_MSK

/* Buffers for MQTT client. */
static u8_t rx_buffer[CONFIG_MQTT_MESSAGE_BUFFER_SIZE];
static u8_t tx_buffer[CONFIG_MQTT_MESSAGE_BUFFER_SIZE];
static u8_t payload_buf[CONFIG_MQTT_PAYLOAD_BUFFER_SIZE];

/* The mqtt client struct */
static struct mqtt_client client;

/* MQTT Broker details. */
static struct sockaddr_storage broker;

/* Connected flag */
static bool connected;

/* Button flag */
static bool isButtonPress;

/* File descriptor */
struct pollfd fds;

/* GPIO PORT */
#define SIM_PORT     DT_GPIO_P0_DEV_NAME
#define SIM_PIN      8

/* Interval in milliseconds between each time status LEDs are updated. */
#define LEDS_UPDATE_INTERVAL            500
#define BUTTONS_UPDATE_INTERVAL         250

/* Structures for work */
static struct k_delayed_work leds_update_work;
static struct k_delayed_work buttons_update_work;

static int data_publish(struct mqtt_client *c, enum mqtt_qos qos, u8_t *data, size_t len);

/**@brief Update BUTTONs state. */
static void buttons_update(struct k_work *work)
{
    if ( connected == true ) {
        data_publish(&client, MQTT_QOS_1_AT_LEAST_ONCE, "Button1pressed", strlen("Button1pressed"));
        //mqtt_disconnect(&client);
    }else{
        mqtt_connect(&client);
        //data_publish(&client, MQTT_QOS_1_AT_LEAST_ONCE, "Button1pressed", strlen("Button1pressed"));
    }
    
//    connected = true;
//
//    int err;
//    err = mqtt_connect(&client);
//    if (err != 0) {
//        printk("ERROR: mqtt_connect %d\n", err);
//        return;
//    }
}

/**@brief Update LEDs state. */
static void leds_update(struct k_work *work)
{
    
    //ARG_UNUSED(work);
    
    for(int i=0; i<3; i++){
        dk_set_led(DK_LED1, 1);
        k_sleep(500);
        dk_set_led(DK_LED1, 0);
        k_sleep(500);
    }
    //k_delayed_work_submit(&leds_update_work, LEDS_UPDATE_INTERVAL);
}

/**@brief Function to print strings without null-termination
 */
static void data_print(u8_t *prefix, u8_t *data, size_t len)
{
    char buf[len + 1];
    
    memcpy(buf, data, len);
    buf[len] = 0;
    printk("%s%s\n", prefix, buf);
    
    k_delayed_work_submit(&leds_update_work, LEDS_UPDATE_INTERVAL);
}

/**@brief Function to publish data on the configured topic
 */
static int data_publish(struct mqtt_client *c, enum mqtt_qos qos,
                        u8_t *data, size_t len)
{
    struct mqtt_publish_param param;
    
    param.message.topic.qos = qos;
    param.message.topic.topic.utf8 = CONFIG_MQTT_PUB_TOPIC;
    param.message.topic.topic.size = strlen(CONFIG_MQTT_PUB_TOPIC);
    param.message.payload.data = data;
    param.message.payload.len = len;
    param.message_id = sys_rand32_get();
    param.dup_flag = 0;
    param.retain_flag = 0;
    
    data_print("Publish: ", data, len);
    printk("to topic: %s len: %u\n",
           CONFIG_MQTT_PUB_TOPIC,
           (unsigned int)strlen(CONFIG_MQTT_PUB_TOPIC));
    
    return mqtt_publish(c, &param);
}

/**@brief Function to subscribe to the configured topic
 */
static int subscribe(void)
{
    struct mqtt_topic subscribe_topic = {
        .topic = {
            .utf8 = CONFIG_MQTT_SUB_TOPIC,
            .size = strlen(CONFIG_MQTT_SUB_TOPIC)
        },
        .qos = MQTT_QOS_1_AT_LEAST_ONCE
    };
    
    const struct mqtt_subscription_list subscription_list = {
        .list = &subscribe_topic,
        .list_count = 1,
        .message_id = 1234
    };
    
    printk("Subscribing to: %s len %u\n", CONFIG_MQTT_SUB_TOPIC,
           (unsigned int)strlen(CONFIG_MQTT_SUB_TOPIC));
    
    return mqtt_subscribe(&client, &subscription_list);
}

/**@brief Function to read the published payload.
 */
static int publish_get_payload(struct mqtt_client *c, size_t length)
{
    u8_t *buf = payload_buf;
    u8_t *end = buf + length;
    
    if (length > sizeof(payload_buf)) {
        return -EMSGSIZE;
    }
    
    while (buf < end) {
        int ret = mqtt_read_publish_payload(c, buf, end - buf);
        
        if (ret < 0) {
            if (ret == -EAGAIN) {
                printk("mqtt_read_publish_payload: EAGAIN");
                poll(&fds, 1, K_FOREVER);
                continue;
            }
            
            return ret;
        }
        
        if (ret == 0) {
            return -EIO;
        }
        
        buf += ret;
    }
    
    return 0;
}

/**@brief MQTT client event handler
 */
void mqtt_evt_handler(struct mqtt_client *const c,
                      const struct mqtt_evt *evt)
{
    int err;
    
    switch (evt->type) {
        case MQTT_EVT_CONNACK:
            if (evt->result != 0) {
                printk("MQTT connect failed %d\n", evt->result);
                break;
            }
            
            connected = true;
            printk("[%s:%d] MQTT client connected!\n", __func__, __LINE__);
            //subscribe();
            
            break;
            
        case MQTT_EVT_DISCONNECT:
            printk("[%s:%d] MQTT client disconnected %d\n", __func__, __LINE__, evt->result);
            connected = false;
            break;
            
        case MQTT_EVT_PUBLISH:
        {
            const struct mqtt_publish_param *p = &evt->param.publish;
            
            printk("[%s:%d] MQTT PUBLISH result=%d len=%d\n", __func__, __LINE__, evt->result, p->message.payload.len);
            err = publish_get_payload(c, p->message.payload.len);
            if (err) {
                
                printk("mqtt_read_publish_payload: Failed! %d\n", err);
                /* Echo back received data */
                //data_publish(&client, MQTT_QOS_1_AT_LEAST_ONCE, payload_buf, p->message.payload.len);
            }
            
            if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
                const struct mqtt_puback_param ack = {
                    .message_id = p->message_id
                };
                
                /* Send acknowledgment. */
                err = mqtt_publish_qos1_ack(c, &ack);
                if (err) {
                    printk("unable to ack\n");
                }
            }
            
            data_print("Received: ", payload_buf, p->message.payload.len);
            //mqtt_disconnect(&client);
        }
            break;
            
        case MQTT_EVT_PUBACK:
            if (evt->result != 0) {
                printk("MQTT PUBACK error %d\n", evt->result);
                break;
            }
            
            printk("[%s:%d] PUBACK packet id: %u\n", __func__, __LINE__, evt->param.puback.message_id);
            isButtonPress = false;
            //mqtt_disconnect(&client);
            break;
            
        case MQTT_EVT_SUBACK:
            if (evt->result != 0) {
                printk("MQTT SUBACK error %d\n", evt->result);
                break;
            }
            
            printk("[%s:%d] SUBACK packet id: %u\n", __func__, __LINE__, evt->param.suback.message_id);
            break;
            
        default:
            printk("[%s:%d] default: %d\n", __func__, __LINE__, evt->type);
            break;
    }
}

/**@brief Resolves the configured hostname and
 * initializes the MQTT broker structure
 */
static void broker_init(void)
{
    int err;
    struct addrinfo *result;
    struct addrinfo *addr;
    struct addrinfo hints;
    
    hints.ai_flags = 0;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = 0;
    
    err = getaddrinfo(CONFIG_MQTT_BROKER_HOSTNAME, NULL, &hints, &result);
    if (err) {
        printk("ERROR: getaddrinfo failed %d\n", err);
        
        return;
    }
    
    addr = result;
    err = -ENOENT;
    
    /* Look for address of the broker. */
    while (addr != NULL) {
        /* IPv4 Address. */
        if (addr->ai_addrlen == sizeof(struct sockaddr_in)) {
            struct sockaddr_in *broker4 =
            ((struct sockaddr_in *)&broker);
            
            broker4->sin_addr.s_addr =
            ((struct sockaddr_in *)addr->ai_addr)
            ->sin_addr.s_addr;
            broker4->sin_family = AF_INET;
            broker4->sin_port = htons(CONFIG_MQTT_BROKER_PORT);
            printk("IPv4 Address found 0x%08x\n", broker4->sin_addr.s_addr);
            break;
        }else if (addr->ai_addrlen == sizeof(struct sockaddr_in6)) {
            /* IPv6 Address. */
            struct sockaddr_in6 *broker6 = ((struct sockaddr_in6 *)&broker);
            memcpy(broker6->sin6_addr.s6_addr, ((struct sockaddr_in6 *)addr->ai_addr)->sin6_addr.s6_addr, sizeof(struct in6_addr));
            
            broker6->sin6_family = AF_INET6;
            broker6->sin6_port = htons(CONFIG_MQTT_BROKER_PORT);
        
            printk("IPv6 Address found 0x%08x\n", broker6->sin6_addr.s6_addr);
            break;
        } else {
            printk("ai_addrlen = %u should be %u or %u\n",
                   (unsigned int)addr->ai_addrlen,
                   (unsigned int)sizeof(struct sockaddr_in),
                   (unsigned int)sizeof(struct sockaddr_in6));
        }
        
        addr = addr->ai_next;
        break;
    }
    
    /* Free the address. */
    freeaddrinfo(result);
}

/**@brief Initialize the MQTT client structure
 */
static void client_init(struct mqtt_client *client)
{
    mqtt_client_init(client);
    
    broker_init();
    
    /* MQTT client configuration */
    client->broker = &broker;
    client->evt_cb = mqtt_evt_handler;
    client->client_id.utf8 = (u8_t *)CONFIG_MQTT_CLIENT_ID;
    client->client_id.size = strlen(CONFIG_MQTT_CLIENT_ID);
    client->password = NULL;
    client->user_name = NULL;
    client->protocol_version = MQTT_VERSION_3_1_1;
    
    /* MQTT buffers configuration */
    client->rx_buf = rx_buffer;
    client->rx_buf_size = sizeof(rx_buffer);
    client->tx_buf = tx_buffer;
    client->tx_buf_size = sizeof(tx_buffer);
    
    /* MQTT transport configuration */
#if defined(CONFIG_MQTT_LIB_TLS)
    struct mqtt_sec_config *tls_config = &client->transport.tls.config;
    
    client->transport.type = MQTT_TRANSPORT_SECURE;
    
    tls_config->peer_verify = 2;
    tls_config->cipher_count = 0;
    tls_config->cipher_list = NULL;
    tls_config->sec_tag_count = ARRAY_SIZE(sec_tag_list);
    tls_config->sec_tag_list = sec_tag_list;
    tls_config->hostname = CONFIG_MQTT_BROKER_HOSTNAME;
#else
    client->transport.type = MQTT_TRANSPORT_NON_SECURE;
#endif

}

/**@brief Initialize the file descriptor structure used by poll.
 */
static int fds_init(struct mqtt_client *c)
{
    if (c->transport.type == MQTT_TRANSPORT_NON_SECURE) {
        fds.fd = c->transport.tcp.sock;
    } else {
#if defined(CONFIG_MQTT_LIB_TLS)
        fds.fd = c->transport.tls.sock;
#else
        return -ENOTSUP;
#endif
    }
    
    fds.events = POLLIN;
    
    return 0;
}

/**@brief Configures modem to provide LTE link. Blocks until link is
 * successfully established.
 */
static void modem_configure(void)
{
#if defined(CONFIG_LTE_LINK_CONTROL)
    if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT)) {
        /* Do nothing, modem is already turned on
         * and connected.
         */
    } else {
        int err;
        
        printk("LTE Link Connecting ...\n");
        err = lte_lc_init_and_connect();
        __ASSERT(err == 0, "LTE link could not be established.");
        printk("LTE Link Connected!\n");
    }
#endif
}

static int provision_certificate(void)
{
#if defined(CONFIG_PROVISION_CERTIFICATES)
#if defined(CONFIG_BSD_LIBRARY)
    {
        int err;
        
        /* Delete certificates */
        nrf_sec_tag_t sec_tag = (nrf_sec_tag_t) sec_tag_list[0];
        
        for (nrf_key_mgnt_cred_type_t type = 0; type < 5; type++) {
            printk("Deleting certs sec_tag: %d\n", sec_tag);
            err = nrf_inbuilt_key_delete(sec_tag, type);
            printk("nrf_inbuilt_key_delete(%u, %d) => result=%d\n", sec_tag, type, err);
        }

#if defined(CA_CERTIFICATE)
        /* Provision CA Certificate. */
        printk("Write ca certs sec_tag: %d\n", sec_tag);
        err = nrf_inbuilt_key_write(sec_tag,
                                    NRF_KEY_MGMT_CRED_TYPE_CA_CHAIN,
                                    CA_CERTIFICATE,
                                    strlen(CA_CERTIFICATE));
        if (err) {
            printk("CA_CERTIFICATE err: %d\n", err);
            return err;
        }
#endif
#if defined (CLIENT_PRIVATE_KEY)
        /* Provision Private Certificate. */
        printk("Write private cert sec_tag: %d\n", sec_tag);
        err = nrf_inbuilt_key_write(
                                    sec_tag,
                                    NRF_KEY_MGMT_CRED_TYPE_PRIVATE_CERT,
                                    CLIENT_PRIVATE_KEY,
                                    strlen(CLIENT_PRIVATE_KEY));
        if (err) {
            printk("CLIENT_PRIVATE_KEY err: %d\n", err);
            return err;
        }
#endif
#if defined(CLIENT_PUBLIC_CERTIFICATE)
        /* Provision Public Certificate. */
        printk("Write public cert sec_tag: %d\n", sec_tag);
        err = nrf_inbuilt_key_write(
                                    sec_tag,
                                    NRF_KEY_MGMT_CRED_TYPE_PUBLIC_CERT,
                                    CLIENT_PUBLIC_CERTIFICATE,
                                    strlen(CLIENT_PUBLIC_CERTIFICATE));
        if (err) {
            printk("CLIENT_PUBLIC_CERTIFICATE err: %d\n",
                   err);
            return err;
        }
    }
#endif
#else
    {
        int err;
        
        err = tls_credential_add(CONFIG_SEC_TAG,
                                 TLS_CREDENTIAL_CA_CERTIFICATE,
                                 NRF_CLOUD_CA_CERTIFICATE,
                                 sizeof(NRF_CLOUD_CA_CERTIFICATE));
        if (err < 0) {
            printk("Failed to register ca certificate: %d\n",
                   err);
            return err;
        }
        err = tls_credential_add(CONFIG_SEC_TAG,
                                 TLS_CREDENTIAL_PRIVATE_KEY,
                                 NRF_CLOUD_CLIENT_PRIVATE_KEY,
                                 sizeof(NRF_CLOUD_CLIENT_PRIVATE_KEY));
        if (err < 0) {
            printk("Failed to register private key: %d\n",
                   err);
            return err;
        }
        err = tls_credential_add(CONFIG_SEC_TAG,
                                 TLS_CREDENTIAL_SERVER_CERTIFICATE,
                                 NRF_CLOUD_CLIENT_PUBLIC_CERTIFICATE,
                                 sizeof(NRF_CLOUD_CLIENT_PUBLIC_CERTIFICATE));
        if (err < 0) {
            printk("Failed to register public certificate: %d\n",
                   err);
            return err;
        }
        
    }
#endif /* defined(CONFIG_BSD_LIBRARY) */
#endif /* defined(CONFIG_PROVISION_CERTIFICATES) */
    
    return 0;
}

/**@brief Initializes and submits delayed work. */
static void work_init(void)
{
    k_delayed_work_init(&leds_update_work, leds_update);
    k_delayed_work_init(&buttons_update_work, buttons_update);
    //k_work_init(&connect_work, cloud_connect);
    k_delayed_work_submit(&leds_update_work, LEDS_UPDATE_INTERVAL);
}


//static void button_push(bool state)
//{
//    if (state) {
//        printk("Button 1\n");
//        if(isButtonPress){
//            ;;
//        }else{
//            isButtonPress = true;
//
//            //k_delayed_work_submit(&buttons_update_work, BUTTONS_UPDATE_INTERVAL);
//            if(!connected) {
//                int err;
//                err = mqtt_connect(&client);
//                if (err != 0) {
//                    printk("ERROR: mqtt_connect %d\n", err);
//                    return;
//                }
//            }
//            k_delayed_work_submit(&buttons_update_work, BUTTONS_UPDATE_INTERVAL);
//        }
//
//        dk_set_led(DK_LED3, 1);
//        k_sleep(1000);
//        dk_set_led(DK_LED3, 0);
//    }
//}


static void button_handler(u32_t button_state, u32_t has_changed)
{
    
    if (has_changed & DK_BTN1_MSK) {
        k_delayed_work_submit(&buttons_update_work, BUTTONS_UPDATE_INTERVAL);
    }else if(has_changed & DK_BTN2_MSK){
    
    }
}

/**@brief Initializes buttons and LEDs, using the DK buttons and LEDs
 * library.
 */
static void buttons_leds_init(void)
{
    int err;
    
    err = dk_buttons_init(button_handler);
    if (err) {
        printk("Could not initialize buttons, err code: %d\n", err);
    }
    
    err = dk_leds_init();
    if (err) {
        printk("Could not initialize leds, err code: %d\n", err);
    }
    
    err = dk_set_leds_state(0x00, DK_ALL_LEDS_MSK);
    if (err) {
        printk("Could not set leds state, err code: %d\n", err);
    }
}

static void sim_pin_select(void)
{
    struct device *gpio_dev;

    gpio_dev = device_get_binding(SIM_PORT);

    if (!gpio_dev) {
      printk("Cannot find %s!\n", "SIM PORT");
      return;
    }

    /* Setup GPIO output */
    int ret = gpio_pin_configure(gpio_dev, SIM_PIN, GPIO_DIR_OUT);
    ret = gpio_pin_write(gpio_dev, SIM_PIN, 0);
}

void main(void)
{
    int err;
    
    //    if (!IS_ENABLED(CONFIG_AT_HOST_LIBRARY)) {
    //        /* Stop the UART RX for power consumption reasons */
    //        NRF_UARTE0_NS->TASKS_STOPRX = 1;
    //        NRF_UARTE1_NS->TASKS_STOPRX = 1;
    //    }
    
    printk("The MQTT AWS IoT Core sample started\n");
    
    buttons_leds_init();

#ifdef CONFIG_BOARD_ACTINIUS_ICARUS 
    sim_pin_select();
#endif
    
#ifdef CONFIG_BOARD_ACTINIUS_ICARUS_NS
    sim_pin_select();
#endif


    work_init();
    provision_certificate();
    modem_configure();
    client_init(&client);

    err = mqtt_connect(&client);
    if (err != 0) {
        printk("ERROR: mqtt_connect %d\n", err);
        return;
    }
    printk("MQTT connectting...\n");

    err = fds_init(&client);
    if (err != 0) {
        printk("ERROR: fds_init %d\n", err);
        return;
    }
    
    while (true) {
        
        //if(connected == true){
          err = mqtt_input(&client);
          if (err != 0) {
            printk("ERROR: mqtt_input %d\n", err);
            
            goto error;
          }

          err = mqtt_live(&client);
          if (err != 0) {
            printk("ERROR: mqtt_live %d\n", err);
            //mqtt_disconnect(&client);
            goto error;
          }

          if (poll(&fds, 1, K_SECONDS(CONFIG_MQTT_KEEPALIVE)) < 0) {
            printk("ERROR: poll %d\n", errno);
          }
        
          k_sleep(K_MSEC(10));
          /* Put CPU to idle to save power */
          k_cpu_idle();
        //}
    }

error:
    return ;
}



