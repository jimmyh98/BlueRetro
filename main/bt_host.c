#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>
#include <esp_system.h>
#include <esp_bt.h>
#include <nvs_flash.h>
#include "bt_host.h"
#include "bt_hci.h"
#include "bt_l2cap.h"
#include "bt_sdp.h"
#include "util.h"
#include "sd.h"

#define H4_TRACE /* Display packet dump that can be parsed by wireshark/text2pcap */

#define BT_TX 0
#define BT_RX 1

typedef void (*bt_cmd_func_t)(void *param);

enum {
    /* BT CTRL flags */
    BT_CTRL_READY,
};

struct bt_name_type {
    char name[249];
    int8_t type;
};

struct bt_hci_cmd_cp {
    bt_cmd_func_t cmd;
    void *cp;
};

struct bt_host_link_keys {
    uint32_t index;
    struct bt_hci_evt_link_key_notify link_keys[16];
} __packed;

struct bt_hci_pkt bt_hci_pkt_tmp;

static struct bt_host_link_keys bt_host_link_keys = {0};
static uint32_t bt_config_state = 0;
static RingbufHandle_t txq_hdl;
static struct bt_dev bt_dev[7] = {0};
static atomic_t bt_flags = 0;

static const struct bt_name_type bt_name_type[] = {
    {"Nintendo RVL-CNT-01-UC", WIIU_PRO},
    {"Nintendo RVL-CNT-01-TR", WII_CORE},
    {"Nintendo RVL-CNT-01", WII_CORE},
    {"Pro Controller", SWITCH_PRO},
    {"Xbox Wireless Controller", XB1_S},
    {"PLAYSTATION(R)3 Controller", PS3_DS3},
    {"Wireless Controller", PS4_DS4},
};

static const struct bt_hci_cp_set_event_filter clr_evt_filter = {
    .filter_type = BT_BREDR_FILTER_TYPE_CLEAR,
};

static const struct bt_hci_cp_set_event_filter inquiry_evt_filter = {
    .filter_type = BT_BREDR_FILTER_TYPE_INQUIRY,
    .condition_type = BT_BDEDR_COND_TYPE_CLASS,
    .inquiry_class.dev_class = {0x00, 0x05, 0x00},
    .inquiry_class.dev_class_mask = {0x00, 0x1F, 0x00},
};

static const struct bt_hci_cp_set_event_filter conn_evt_filter = {
    .filter_type = BT_BREDR_FILTER_TYPE_CONN,
    .condition_type = BT_BDEDR_COND_TYPE_CLASS,
    .conn_class.dev_class = {0x00, 0x05, 0x00},
    .conn_class.dev_class_mask = {0x00, 0x1F, 0x00},
    .conn_class.auto_accept_flag =  BT_BREDR_AUTO_OFF,
};

static const struct bt_hci_cmd_cp bt_hci_config[] = {
    {bt_hci_cmd_reset, NULL},
    {bt_hci_cmd_read_local_features, NULL},
    {bt_hci_cmd_read_local_version_info, NULL},
    {bt_hci_cmd_read_bd_addr, NULL},
    {bt_hci_cmd_read_buffer_size, NULL},
    {bt_hci_cmd_read_class_of_device, NULL},
    {bt_hci_cmd_read_local_name, NULL},
    {bt_hci_cmd_read_voice_setting, NULL},
    {bt_hci_cmd_read_num_supported_iac, NULL},
    {bt_hci_cmd_read_current_iac_lap, NULL},
    {bt_hci_cmd_set_event_filter, (void *)&clr_evt_filter},
    {bt_hci_cmd_write_conn_accept_timeout, NULL},
    {bt_hci_cmd_read_supported_commands, NULL},
    {bt_hci_cmd_write_ssp_mode, NULL},
    {bt_hci_cmd_write_inquiry_mode, NULL},
    {bt_hci_cmd_read_inquiry_rsp_tx_pwr_lvl, NULL},
    {bt_hci_cmd_read_local_ext_features, NULL},
    {bt_hci_cmd_read_stored_link_key, NULL},
    {bt_hci_cmd_read_page_scan_activity, NULL},
    {bt_hci_cmd_read_page_scan_type, NULL},
    {bt_hci_cmd_write_le_host_supp, NULL},
    {bt_hci_cmd_delete_stored_link_key, NULL},
    {bt_hci_cmd_write_class_of_device, NULL},
    {bt_hci_cmd_write_local_name, NULL},
    {bt_hci_cmd_set_event_filter, (void *)&inquiry_evt_filter},
    {bt_hci_cmd_set_event_filter, (void *)&conn_evt_filter},
    {bt_hci_cmd_write_auth_enable, NULL},
    {bt_hci_cmd_set_event_mask, NULL},
    {bt_hci_cmd_write_page_scan_activity, NULL},
    {bt_hci_cmd_write_inquiry_scan_activity, NULL},
    {bt_hci_cmd_write_page_scan_type, NULL},
    {bt_hci_cmd_write_page_scan_timeout, NULL},
    {bt_hci_cmd_write_hold_mode_act, NULL},
    {bt_hci_cmd_write_scan_enable, NULL},
    {bt_hci_cmd_write_default_link_policy, NULL},
    {bt_hci_cmd_periodic_inquiry, NULL},
};

#ifdef H4_TRACE
static void bt_h4_trace(uint8_t *data, uint16_t len, uint8_t dir);
#endif /* H4_TRACE */
static void bt_host_acl_hdlr(struct bt_hci_pkt *bt_hci_acl_pkt);
static void bt_host_tx_pkt_ready(void);
static int bt_host_rx_pkt(uint8_t *data, uint16_t len);
static void bt_host_tx_ringbuf_task(void *param);

static esp_vhci_host_callback_t vhci_host_cb = {
    bt_host_tx_pkt_ready,
    bt_host_rx_pkt
};

#ifdef H4_TRACE
static void bt_h4_trace(uint8_t *data, uint16_t len, uint8_t dir) {
    uint8_t col;
    uint16_t byte, line;
    uint16_t line_max = len/16;

    if (len % 16)
        line_max++;

    if (dir)
        printf("I ");
    else
        printf("O ");

    for (byte = 0, line = 0; line < line_max; line++) {
        printf("%06X", byte);
        for (col = 0; col < 16 && byte < len; col++, byte++) {
            printf(" %02X", data[byte]);
        }
        printf("\n");
    }
}
#endif /* H4_TRACE */

static void bt_host_tx_ringbuf_task(void *param) {
    size_t packet_len;
    uint8_t *packet;

    while(1) {
        if (atomic_test_bit(&bt_flags, BT_CTRL_READY)) {
            packet = (uint8_t *)xRingbufferReceive(txq_hdl, &packet_len, 0);
            if (packet) {
                if (packet[0] == 0xFF) {
                    /* Internal wait packet */
                    vTaskDelay(packet[1] / portTICK_PERIOD_MS);
                }
                else {
#ifdef H4_TRACE
                    bt_h4_trace(packet, packet_len, BT_TX);
#endif /* H4_TRACE */
                    atomic_clear_bit(&bt_flags, BT_CTRL_READY);
                    esp_vhci_host_send_packet(packet, packet_len);
                }
                vRingbufferReturnItem(txq_hdl, (void *)packet);
            }
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

static void bt_host_acl_hdlr(struct bt_hci_pkt *bt_hci_acl_pkt) {
    struct bt_dev *device = NULL;
    bt_host_get_dev_from_handle(bt_hci_acl_pkt->acl_hdr.handle, &device);

    if (device == NULL) {
        printf("# dev NULL!\n");
        return;
    }

    if (bt_hci_acl_pkt->l2cap_hdr.cid == BT_L2CAP_CID_BR_SIG) {
        bt_l2cap_sig_hdlr(device, bt_hci_acl_pkt);
    }
    else if (bt_hci_acl_pkt->l2cap_hdr.cid == device->sdp_tx_chan.scid ||
        bt_hci_acl_pkt->l2cap_hdr.cid == device->sdp_rx_chan.scid) {
        bt_sdp_hdlr(device, bt_hci_acl_pkt);
    }
    else if (bt_hci_acl_pkt->l2cap_hdr.cid == device->ctrl_chan.scid ||
        bt_hci_acl_pkt->l2cap_hdr.cid == device->intr_chan.scid) {
        bt_hid_hdlr(device, bt_hci_acl_pkt);
    }
}

/*
 * @brief: BT controller callback function, used to notify the upper layer that
 *         controller is ready to receive command
 */
static void bt_host_tx_pkt_ready(void) {
    atomic_set_bit(&bt_flags, BT_CTRL_READY);
}

/*
 * @brief: BT controller callback function, to transfer data packet to upper
 *         controller is ready to receive command
 */
static int bt_host_rx_pkt(uint8_t *data, uint16_t len) {
    struct bt_hci_pkt *bt_hci_pkt = (struct bt_hci_pkt *)data;
#ifdef H4_TRACE
    bt_h4_trace(data, len, BT_RX);
#endif /* H4_TRACE */

    switch(bt_hci_pkt->h4_hdr.type) {
        case BT_HCI_H4_TYPE_ACL:
            bt_host_acl_hdlr(bt_hci_pkt);
            break;
        case BT_HCI_H4_TYPE_EVT:
            bt_hci_evt_hdlr(bt_hci_pkt);
            break;
        default:
            printf("# %s unsupported packet type: 0x%02X\n", __FUNCTION__, bt_hci_pkt->h4_hdr.type);
            break;
    }

    return 0;
}

int32_t bt_host_get_new_dev(struct bt_dev **device) {
    for (uint32_t i = 0; i < 7; i++) {
        if (!atomic_test_bit(&bt_dev[i].flags, BT_DEV_DEVICE_FOUND)) {
            *device = &bt_dev[i];
            return i;
        }
    }
    return -1;
}

int32_t bt_host_get_active_dev(struct bt_dev **device) {
    for (uint32_t i = 0; i < 7; i++) {
        if (atomic_test_bit(&bt_dev[i].flags, BT_DEV_DEVICE_FOUND)) {
            *device = &bt_dev[i];
            return i;
        }
    }
    return -1;
}

int32_t bt_host_get_dev_from_bdaddr(bt_addr_t *bdaddr, struct bt_dev **device) {
    for (uint32_t i = 0; i < 7; i++) {
        if (memcmp((void *)bdaddr, bt_dev[i].remote_bdaddr, 6) == 0) {
            *device = &bt_dev[i];
            return i;
        }
    }
    return -1;
}

int32_t bt_host_get_dev_from_handle(uint16_t handle, struct bt_dev **device) {
    for (uint32_t i = 0; i < 7; i++) {
        if (bt_acl_handle(handle) == bt_dev[i].acl_handle) {
            *device = &bt_dev[i];
            return i;
        }
    }
    return -1;
}

int32_t bt_host_get_type_from_name(const uint8_t* name) {
    for (uint32_t i = 0; i < sizeof(bt_name_type)/sizeof(*bt_name_type); i++) {
        if (memcmp(name, bt_name_type[i].name, strlen(bt_name_type[i].name)) == 0) {
            return bt_name_type[i].type;
        }
    }
    return -1;
}

void bt_host_reset_dev(struct bt_dev *device) {
    memset((void *)device, 0, sizeof(*device));
}

void bt_host_restart_config(void) {
    bt_config_state = 0;
}

void bt_host_config_q_cmd(uint32_t next) {
    if (next) {
        bt_config_state++;
    }
    if (bt_config_state < ARRAY_SIZE(bt_hci_config)) {
        bt_hci_config[bt_config_state].cmd(bt_hci_config[bt_config_state].cp);
    }
}

void bt_host_q_wait_pkt(uint32_t ms) {
    uint8_t packet[2] = {0xFF, ms};

    bt_host_txq_add(packet, sizeof(packet));
}

int32_t bt_host_init(void) {
    uint8_t test_mac[6] = {0x84, 0x4b, 0xf5, 0xa7, 0x41, 0xea};

    /* Initialize NVS — it is used to store PHY calibration data */
    int32_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_base_mac_addr_set(test_mac);

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    if ((ret = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        printf("Bluetooth controller initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if ((ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
        printf("Bluetooth controller enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_vhci_host_register_callback(&vhci_host_cb);

    bt_host_tx_pkt_ready();

    txq_hdl = xRingbufferCreate(256*8, RINGBUF_TYPE_NOSPLIT);
    if (txq_hdl == NULL) {
        printf("Failed to create ring buffer\n");
        return ret;
    }

    sd_load_link_keys((uint8_t *)&bt_host_link_keys, sizeof(bt_host_link_keys));

    xTaskCreatePinnedToCore(&bt_host_tx_ringbuf_task, "bt_host_tx_task", 2048, NULL, 5, NULL, 0);

    bt_host_config_q_cmd(0);

    return ret;
}

int32_t bt_host_txq_add(uint8_t *packet, uint32_t packet_len) {
    UBaseType_t ret = xRingbufferSend(txq_hdl, (void *)packet, packet_len, 0);
    if (ret != pdTRUE) {
        printf("# %s txq full!\n", __FUNCTION__);
    }
    return (ret == pdTRUE ? 0 : -1);
}

int32_t bt_host_load_link_key(struct bt_hci_cp_link_key_reply *link_key_reply) {
    int32_t ret = -1;
    for (uint32_t i = 0; i < ARRAY_SIZE(bt_host_link_keys.link_keys); i++) {
        if (memcmp((void *)&link_key_reply->bdaddr, (void *)&bt_host_link_keys.link_keys[i].bdaddr, sizeof(link_key_reply->bdaddr)) == 0) {
            memcpy((void *)link_key_reply->link_key, &bt_host_link_keys.link_keys[i].link_key, sizeof(link_key_reply->link_key));
            ret = 0;
        }
    }
    return ret;
}

int32_t bt_host_store_link_key(struct bt_hci_evt_link_key_notify *link_key_notify) {
    int32_t ret = -1;
    uint32_t index = bt_host_link_keys.index;
    for (uint32_t i = 0; i < ARRAY_SIZE(bt_host_link_keys.link_keys); i++) {
        if (memcmp((void *)&link_key_notify->bdaddr, (void *)&bt_host_link_keys.link_keys[i].bdaddr, sizeof(link_key_notify->bdaddr)) == 0) {
            index = i;
        }
    }
    memcpy((void *)&bt_host_link_keys.link_keys[index], (void *)link_key_notify, sizeof(bt_host_link_keys.link_keys[0]));
    if (index == bt_host_link_keys.index) {
        bt_host_link_keys.index++;
        bt_host_link_keys.index &= 0xF;
    }
    ret = sd_store_link_keys((uint8_t *)&bt_host_link_keys, sizeof(bt_host_link_keys));
    return ret;
}

void bt_host_bridge(struct bt_dev *device, uint8_t *data, uint32_t len) {
    memcpy(bt_adapter.data[device->id].input, data, len);
    adapter_bridge(device->id);
    if (atomic_test_bit(&bt_adapter.data[device->id].flags, BT_FEEDBACK)) {
        bt_hid_feedback(device, bt_adapter.data[device->id].output);
        atomic_clear_bit(&bt_adapter.data[device->id].flags, BT_FEEDBACK);
    }
}
