/*
 * Copyright (c) 2020 Nanjing Xiaoxiongpai Intelligent Technology Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cmsis_os2.h"
#include "ohos_init.h"


#include <dtls_al.h>
#include <mqtt_al.h>
#include <oc_mqtt_al.h>
#include <oc_mqtt_profile.h>
#include "E53_IS1.h"
#include "wifi_connect.h"
#include "iot_adc.h"
#include "iot_errno.h"
#include "iot_gpio.h"
#include "iot_gpio_ex.h"

#define CONFIG_WIFI_SSID "vivo" // 修改为自己的WiFi 热点账号

#define CONFIG_WIFI_PWD "12345678" // 修改为自己的WiFi 热点密码

#define CONFIG_APP_SERVERIP "121.36.42.100"

#define CONFIG_APP_SERVERPORT "1883"

#define CONFIG_APP_DEVICEID "64883e9501554a5933a0a584_1234" // 替换为注册设备后生成的deviceid

#define CONFIG_APP_DEVICEPWD "123123123" // 替换为注册设备后生成的密钥

#define CONFIG_APP_LIFETIME 60 // seconds

#define CONFIG_QUEUE_TIMEOUT (5 * 1000)

#define MSGQUEUE_COUNT 16
#define MSGQUEUE_SIZE 10
#define CLOUD_TASK_STACK_SIZE (1024 * 10)
#define CLOUD_TASK_PRIO 24
#define BEEP_DELAY 2
#define CURRENT_OVERLOADED 2

#define ADC_VREF_VOL 1.8
#define ADC_COEFFICIENT 4
#define ADC_RATIO 4096
#define R 50

#define ADC_CHANNEL 4

#define IOT_UART_IDX_1 1
#define UART_BUFF_SIZE 1544

typedef enum
{
    en_msg_cmd = 0,
    en_msg_report,
} en_msg_type_t;

typedef struct
{
    char *request_id;
    char *payload;
} cmd_t;

typedef struct
{
    osMessageQueueId_t app_msg;
    int connected;
} app_cb_t;
static app_cb_t g_app_cb;

int g_infraredStatus = 0;
float current_status = 0;
char infra_pic_status[2 * UART_BUFF_SIZE+1] = "";
uint8_t infra_pic_status_temp[UART_BUFF_SIZE] = "";
int on_off = 0;

/**
 * @brief 消息上云
 *
 */
static void deal_report_msg(void)
{
    oc_mqtt_profile_service_t service;
    oc_mqtt_profile_kv_t Infrared_status;
    oc_mqtt_profile_kv_t Current;
    oc_mqtt_profile_kv_t infra_pic;
    if (g_app_cb.connected != 1)
    {
        return;
    }
    service.event_time = NULL;
    service.service_id = "safety";//设备安全状态
    service.service_property = &Infrared_status;
    service.nxt = NULL;

    Infrared_status.key = "Infrared_Status";
    Infrared_status.value = g_infraredStatus ? "Intrude" : "Safe";
    Infrared_status.type = EN_OC_MQTT_PROFILE_VALUE_STRING;
    Infrared_status.nxt = &Current;

    Current.key = "Current";//电流值
    Current.value = &current_status;
    Current.type = EN_OC_MQTT_PROFILE_VALUE_FLOAT;
    Current.nxt = &infra_pic;

    infra_pic.key = "infra_pic";//温度图字符串
    infra_pic.value = infra_pic_status;
    infra_pic.type = EN_OC_MQTT_PROFILE_VALUE_STRING;
    infra_pic.nxt = NULL;

    oc_mqtt_profile_propertyreport(NULL, &service);
    return;
}

/**
 * @brief Callback for F1 key
 *
 */
static void F1Pressed(char *arg)
{
    (void)arg;
    // IoTGpioSetOutputVal(LED_GPIO, 1);

    IoTGpioSetOutputVal(open_GPIO, 1);
}

/**
 * @brief Callback for F2 key
 *
 */
static void F2Pressed(char *arg)
{
    (void)arg;
    // IoTGpioSetOutputVal(LED_GPIO, 0);

    IoTGpioSetOutputVal(open_GPIO, 0);
}
static void Overcurrent(char *arg)
{
    (void)arg;
    IoTGpioSetOutputVal(open_GPIO, 1);
    printf("Over Current!");
}

/**
 * @brief get ADC sampling value and convert it to voltage
 *
 */
static float GetVoltage(void)
{
    unsigned int ret;
    unsigned short data;

    ret = IoTAdcRead(ADC_CHANNEL, &data, IOT_ADC_EQU_MODEL_8, IOT_ADC_CUR_BAIS_DEFAULT, 0xff);
    if (ret != IOT_SUCCESS)
    {
        printf("ADC Read Fail\n");
    }

    return (float)data * ADC_VREF_VOL * ADC_COEFFICIENT / ADC_RATIO / R;
}

#define FLAGS_MSK1 0x00000001U

osEventFlagsId_t g_eventFlagsId;

static void BeepAlarm(char *arg)
{
    (void)arg;
    osEventFlagsSet(g_eventFlagsId, FLAGS_MSK1);
    printf("triggered.\n");
}

static int CloudMainTaskEntry(void)
{
    printf("Stage 0.\n");
    uint32_t ret;
    WifiConnect(CONFIG_WIFI_SSID, CONFIG_WIFI_PWD);
    printf("Stage 1.\n");
    dtls_al_init();
    printf("Stage 2.\n");
    mqtt_al_init();
    printf("Stage 3.\n");
    oc_mqtt_init();
    printf("Stage 4.\n");

    IoTGpioInit(BUTTON_F1_GPIO);
    IoTGpioSetFunc(BUTTON_F1_GPIO, IOT_GPIO_FUNC_GPIO_11_GPIO);
    IoTGpioSetDir(BUTTON_F1_GPIO, IOT_GPIO_DIR_IN);
    IoTGpioSetPull(BUTTON_F1_GPIO, IOT_GPIO_PULL_UP);
    IoTGpioRegisterIsrFunc(BUTTON_F1_GPIO, IOT_INT_TYPE_EDGE, IOT_GPIO_EDGE_RISE_LEVEL_HIGH, F1Pressed, NULL);

    IoTGpioInit(BUTTON_F2_GPIO);
    IoTGpioSetFunc(BUTTON_F2_GPIO, IOT_GPIO_FUNC_GPIO_12_GPIO);
    IoTGpioSetDir(BUTTON_F2_GPIO, IOT_GPIO_DIR_IN);
    IoTGpioSetPull(BUTTON_F2_GPIO, IOT_GPIO_PULL_UP);
    IoTGpioRegisterIsrFunc(BUTTON_F2_GPIO, IOT_INT_TYPE_EDGE, IOT_GPIO_EDGE_RISE_LEVEL_HIGH, F2Pressed, NULL);

    g_app_cb.app_msg = osMessageQueueNew(MSGQUEUE_COUNT, MSGQUEUE_SIZE, NULL);
    if (g_app_cb.app_msg == NULL)
    {
        printf("Create receive msg queue failed");
    }
    oc_mqtt_profile_connect_t connect_para;
    (void)memset_s(&connect_para, sizeof(connect_para), 0, sizeof(connect_para));

    connect_para.boostrap = 0;
    connect_para.device_id = CONFIG_APP_DEVICEID;
    connect_para.device_passwd = CONFIG_APP_DEVICEPWD;
    connect_para.server_addr = CONFIG_APP_SERVERIP;
    connect_para.server_port = CONFIG_APP_SERVERPORT;
    connect_para.life_time = CONFIG_APP_LIFETIME;
    connect_para.rcvfunc = NULL;
    connect_para.security.type = EN_DTLS_AL_SECURITY_TYPE_NONE;
    ret = oc_mqtt_profile_connect(&connect_para);
    if ((ret == (int)en_oc_mqtt_err_ok))
    {
        g_app_cb.connected = 1;
        printf("oc_mqtt_profile_connect succeeded!\r\n");
    }
    else
    {
        printf("oc_mqtt_profile_connect failed!\r\n");
    }

    printf("Stage 5.\n");
    E53IS1Init();
    ret = E53IS1ReadData(BeepAlarm);
    if (ret != 0)
    {
        printf("E53_IS1 Read Data failed!\r\n");
        return;
    }
    deal_report_msg();
    while (1)
    {
        osEventFlagsWait(g_eventFlagsId, FLAGS_MSK1, osFlagsWaitAny, osWaitForever); // 线程阻塞直至检测到红外信号
        BeepStatusSet(ON);                                                           // 蜂鸣器启动
        g_infraredStatus = 1;                                                        // 修改设备安全状态
        IoTGpioSetOutputVal(open_GPIO, 1);

        deal_report_msg(); // 信息上报

        sleep(BEEP_DELAY);

        BeepStatusSet(OFF);
        g_infraredStatus = 0;
        // if (on_off == 1)
        //     IoTGpioSetOutputVal(open_GPIO, 0);
        deal_report_msg();
    }
    return 0;
}

/**
 * @brief 用于上传电流值的线程
 *
 */
static void upload_current(void)
{
    while (1)
    {
        sleep(1);
        current_status = GetVoltage();//读取电流
        if(current_status>2.0)
            {
                IoTGpioSetOutputVal(BUTTON_F1_GPIO, IOT_GPIO_VALUE1);//因为按键1与过流效果相同，故直接采用按键1中断
                usleep(3000);
                IoTGpioSetOutputVal(BUTTON_F1_GPIO, IOT_GPIO_VALUE0);//先高电平在低电平，中断模式是下降沿
            }
        deal_report_msg();//上报
    }
}
static void readpic(void)
{
    while (1)
    {
        sleep(1);
        IoTUartRead(IOT_UART_IDX_1, infra_pic_status_temp, UART_BUFF_SIZE);//串口数据读取
         for (int i = 0; i < 1544; i++)
            { 
                char c1,c2;
                

                c1 = infra_pic_status_temp[i] & 0xFu;
                c2 = (infra_pic_status_temp[i] >> 4) & 0xFu;
                sprintf(infra_pic_status + i * 2, "%x%x", c2, c1);
            }
        deal_report_msg();//上报
    }
}
static void IotMainTaskEntry(void)
{
    g_eventFlagsId = osEventFlagsNew(NULL);
    if (g_eventFlagsId == NULL)
    {
        printf("Failed to create EventFlags!\n");
    }
    osThreadAttr_t attr;

    attr.name = "CloudMainTaskEntry";
    attr.attr_bits = 0U;
    attr.cb_mem = NULL;
    attr.cb_size = 0U;
    attr.stack_mem = NULL;
    attr.stack_size = CLOUD_TASK_STACK_SIZE;
    attr.priority = CLOUD_TASK_PRIO;

    if (osThreadNew((osThreadFunc_t)CloudMainTaskEntry, NULL, &attr) == NULL)
    {
        printf("Failed to create CloudMainTaskEntry!\n");
    }
    if (osThreadNew((osThreadFunc_t)upload_current, NULL, &attr) == NULL)
    {
        printf("Failed to create upload_current!\n");
    }
    if (osThreadNew((osThreadFunc_t)readpic, NULL, &attr) == NULL)
    {
        printf("Failed to create readpic\n");
    }
}

APP_FEATURE_INIT(IotMainTaskEntry);