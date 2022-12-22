/*
 * Copyright (C) 2015-2021 Espressif Systems (Shanghai) PTE LTD
 *
 * This software file (the "File") is distributed by Espressif Systems (Shanghai)
 * PTE LTD under the terms of the GNU General Public License Version 2, June 1991
 * (the "License").  You may use, redistribute and/or modify this File in
 * accordance with the terms and conditions of the License, a copy of which
 * is available by writing to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA or on the
 * worldwide web at http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt.
 *
 * THE FILE IS DISTRIBUTED AS-IS, WITHOUT WARRANTY OF ANY KIND, AND THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE
 * ARE EXPRESSLY DISCLAIMED.  The License provides additional details about
 * this warranty disclaimer.
 */
#include <linux/device.h>
#include <linux/spi/spi.h>
#include <linux/gpio.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include "esp_spi.h"
#include "esp_if.h"
#include "esp_api.h"
#include "esp_bt_api.h"
#ifdef CONFIG_SUPPORT_ESP_SERIAL
#include "esp_serial.h"
#endif

#define SPI_INITIAL_CLK_MHZ     10
#define NUMBER_1M               1000000
#define TX_MAX_PENDING_COUNT    100
#define TX_RESUME_THRESHOLD     (TX_MAX_PENDING_COUNT/5)

/* ESP in sdkconfig has CONFIG_IDF_FIRMWARE_CHIP_ID entry.
 * supported values of CONFIG_IDF_FIRMWARE_CHIP_ID are - */
#define ESP_PRIV_FIRMWARE_CHIP_UNRECOGNIZED (0xff)
#define ESP_PRIV_FIRMWARE_CHIP_ESP32        (0x0)
#define ESP_PRIV_FIRMWARE_CHIP_ESP32S2      (0x2)
#define ESP_PRIV_FIRMWARE_CHIP_ESP32C3      (0x5)

static struct sk_buff *read_packet(struct esp_adapter *adapter);
static int write_packet(struct esp_adapter *adapter, struct sk_buff *skb);
static void spi_exit(void);
static void adjust_spi_clock(u8 spi_clk_mhz);

volatile u8 data_path = 0;
static struct esp_spi_context spi_context;
static char hardware_type = 0;
static atomic_t tx_pending;

static struct esp_if_ops if_ops = {
    .read = read_packet,
    .write = write_packet,
};

static DEFINE_MUTEX(spi_lock);

static void open_data_path(void)
{
    atomic_set(&tx_pending, 0);
    msleep(200);
    data_path = OPEN_DATAPATH;
}

static void close_data_path(void)
{
    data_path = CLOSE_DATAPATH;
    msleep(200);
}

static irqreturn_t spi_data_ready_interrupt_handler(int irq, void *dev)
{
    /* ESP peripheral has queued buffer for transmission */
    if (spi_context.spi_workqueue) {
        queue_work(spi_context.spi_workqueue, &spi_context.spi_work);
    }

    return IRQ_HANDLED;
}

static irqreturn_t spi_interrupt_handler(int irq, void *dev)
{
    /* ESP peripheral is ready for next SPI transaction */
    if (spi_context.spi_workqueue) {
        queue_work(spi_context.spi_workqueue, &spi_context.spi_work);
    }

    return IRQ_HANDLED;
}

static struct sk_buff *read_packet(struct esp_adapter *adapter)
{
    struct esp_spi_context *context;
    struct sk_buff *skb = NULL;

    if (!data_path) {
        return NULL;
    }

    if (!adapter || !adapter->if_context) {
        printk(KERN_ERR "%s: Invalid args\n", __func__);
        return NULL;
    }

    context = adapter->if_context;

    if (context->esp_spi_dev) {
        skb = skb_dequeue(&(context->rx_q[PRIO_Q_OTHERS]));
    } else {
        printk(KERN_ERR "%s: Invalid args\n", __func__);
        return NULL;
    }

    return skb;
}

static int write_packet(struct esp_adapter *adapter, struct sk_buff *skb)
{
    u32 max_pkt_size = SPI_BUF_SIZE - sizeof(struct esp_payload_header);
    struct esp_payload_header *payload_header = (struct esp_payload_header *)skb->data;

    if (!adapter || !adapter->if_context || !skb || !skb->data || !skb->len) {
        printk(KERN_ERR "%s: Invalid args\n", __func__);
        dev_kfree_skb(skb);
        return -EINVAL;
    }

    if (skb->len > max_pkt_size) {
        printk(KERN_ERR "%s: Drop pkt of len[%u] > max spi transport len[%u]\n",
               __func__, skb->len, max_pkt_size);
        dev_kfree_skb(skb);
        return -EPERM;
    }

    if (!data_path) {
        dev_kfree_skb(skb);
        return -EPERM;
    }

    /* Enqueue SKB in tx_q */
    if (payload_header->if_type == ESP_HCI_IF) {
        skb_queue_tail(&spi_context.tx_q[PRIO_Q_BT], skb);
    } else {
        if (atomic_read(&tx_pending) >= TX_MAX_PENDING_COUNT) {
            esp_tx_pause();
            dev_kfree_skb(skb);

            if (spi_context.spi_workqueue) {
                queue_work(spi_context.spi_workqueue, &spi_context.spi_work);
            }

            return -EBUSY;
        }

        skb_queue_tail(&spi_context.tx_q[PRIO_Q_OTHERS], skb);
        atomic_inc(&tx_pending);
    }

    if (spi_context.spi_workqueue) {
        queue_work(spi_context.spi_workqueue, &spi_context.spi_work);
    }

    return 0;
}


void process_init_event(u8 *evt_buf, u8 len)
{
    u8 len_left = len, tag_len;
    u8 *pos;

    if (!evt_buf) {
        return;
    }

    pos = evt_buf;

    while (len_left) {
        tag_len = *(pos + 1);
        printk(KERN_INFO "EVENT: %d\n", *pos);

        if (*pos == ESP_PRIV_CAPABILITY) {
            process_capabilities(*(pos + 2));
        } else if (*pos == ESP_PRIV_SPI_CLK_MHZ) {
            adjust_spi_clock(*(pos + 2));
        } else if (*pos == ESP_PRIV_FIRMWARE_CHIP_ID) {
            hardware_type = *(pos + 2);
        } else {
            printk(KERN_WARNING "Unsupported tag in event");
        }

        pos += (tag_len + 2);
        len_left -= (tag_len + 2);
    }

    if ((hardware_type != ESP_PRIV_FIRMWARE_CHIP_ESP32) &&
            (hardware_type != ESP_PRIV_FIRMWARE_CHIP_ESP32S2) &&
            (hardware_type != ESP_PRIV_FIRMWARE_CHIP_ESP32C3)) {
        printk(KERN_INFO "ESP board type is not mentioned, ignoring [%d]\n", hardware_type);
        hardware_type = ESP_PRIV_FIRMWARE_CHIP_UNRECOGNIZED;
    }
}


static int process_rx_buf(struct sk_buff *skb)
{
    struct esp_payload_header *header;
    u16 len = 0;
    u16 offset = 0;

    if (!skb) {
        return -EINVAL;
    }

    header = (struct esp_payload_header *)skb->data;

    if (header->if_type >= ESP_MAX_IF) {
        return -EINVAL;
    }

    offset = le16_to_cpu(header->offset);

    /* Validate received SKB. Check len and offset fields */
    if (offset != sizeof(struct esp_payload_header)) {
        return -EINVAL;
    }

    len = le16_to_cpu(header->len);

    if (!len) {
        return -EINVAL;
    }

    len += sizeof(struct esp_payload_header);

    if (len > SPI_BUF_SIZE) {
        return -EINVAL;
    }

    /* Trim SKB to actual size */
    skb_trim(skb, len);


    if (!data_path) {
        return -EPERM;
    }

    /* enqueue skb for read_packet to pick it */
    if (header->if_type == ESP_HCI_IF) {
        skb_queue_tail(&spi_context.rx_q[PRIO_Q_BT], skb);
    } else {
        skb_queue_tail(&spi_context.rx_q[PRIO_Q_OTHERS], skb);
    }

    /* indicate reception of new packet */
    esp_process_new_packet_intr(spi_context.adapter);

    return 0;
}

static void esp_spi_work(struct work_struct *work)
{
    struct spi_transfer trans;
    struct sk_buff *tx_skb = NULL, * rx_skb = NULL;
    u8 *rx_buf;
    int ret = 0;
    volatile int trans_ready, rx_pending;

    mutex_lock(&spi_lock);

    trans_ready = gpio_get_value(HANDSHAKE_PIN);
    rx_pending = gpio_get_value(SPI_DATA_READY_PIN);

    if (trans_ready) {
        if (data_path) {
            tx_skb = skb_dequeue(&spi_context.tx_q[PRIO_Q_OTHERS]);

            if (tx_skb) {
                if (atomic_read(&tx_pending)) {
                    atomic_dec(&tx_pending);
                }

                if (atomic_read(&tx_pending) < TX_RESUME_THRESHOLD) {
                    esp_tx_resume();
                }
            }
        }

        if (rx_pending || tx_skb) {
            memset(&trans, 0, sizeof(trans));

            /* Setup and execute SPI transaction
             *     Tx_buf: Check if tx_q has valid buffer for transmission,
             *         else keep it blank
             *
             *     Rx_buf: Allocate memory for incoming data. This will be freed
             *        immediately if received buffer is invalid.
             *        If it is a valid buffer, upper layer will free it.
             * */

            /* Configure TX buffer if available */

            if (tx_skb) {
                trans.tx_buf = tx_skb->data;
            } else {
                tx_skb = esp_alloc_skb(SPI_BUF_SIZE);
                trans.tx_buf = skb_put(tx_skb, SPI_BUF_SIZE);
                memset((void *)trans.tx_buf, 0, SPI_BUF_SIZE);
            }

            /* Configure RX buffer */
            rx_skb = esp_alloc_skb(SPI_BUF_SIZE);
            rx_buf = skb_put(rx_skb, SPI_BUF_SIZE);

            memset(rx_buf, 0, SPI_BUF_SIZE);

            trans.rx_buf = rx_buf;
            trans.len = SPI_BUF_SIZE;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 15, 0))

            if (hardware_type == ESP_PRIV_FIRMWARE_CHIP_ESP32) {
                trans.cs_change = 1;
            }

#endif

            ret = spi_sync_transfer(spi_context.esp_spi_dev, &trans, 1);

            if (ret) {
                printk(KERN_ERR "SPI Transaction failed: %d", ret);
                dev_kfree_skb(rx_skb);
                dev_kfree_skb(tx_skb);
            } else {

                /* Free rx_skb if received data is not valid */
                if (process_rx_buf(rx_skb)) {
                    dev_kfree_skb(rx_skb);
                }

                if (tx_skb) {
                    dev_kfree_skb(tx_skb);
                }
            }
        }
    }

    mutex_unlock(&spi_lock);
}

static int spi_dev_init(int spi_clk_mhz)
{
    int status = 0;
    struct spi_board_info esp_board = { {0} };
    struct spi_master *master = NULL;

    strlcpy(esp_board.modalias, "esp_spi", sizeof(esp_board.modalias));
    esp_board.mode = SPI_MODE_2;
    esp_board.max_speed_hz = spi_clk_mhz * NUMBER_1M;
    esp_board.bus_num = 0;
    esp_board.chip_select = 0;

    master = spi_busnum_to_master(esp_board.bus_num);

    if (!master) {
        printk(KERN_ERR "Failed to obtain SPI master handle\n");
        return -ENODEV;
    }

    spi_context.esp_spi_dev = spi_new_device(master, &esp_board);

    if (!spi_context.esp_spi_dev) {
        printk(KERN_ERR "Failed to add new SPI device\n");
        return -ENODEV;
    }

    status = spi_setup(spi_context.esp_spi_dev);

    if (status) {
        printk(KERN_ERR "Failed to setup new SPI device");
        return status;
    }

    printk(KERN_INFO "ESP32 peripheral is registered to SPI bus [%d]"
           ",chip select [%d], SPI Clock [%d]\n", esp_board.bus_num,
           esp_board.chip_select, spi_clk_mhz);

    status = gpio_request(HANDSHAKE_PIN, "SPI_HANDSHAKE_PIN");

    if (status) {
        printk(KERN_ERR "Failed to obtain GPIO for Handshake pin, err:%d\n", status);
        return status;
    }

    status = gpio_direction_input(HANDSHAKE_PIN);

    if (status) {
        printk(KERN_ERR "Failed to set GPIO direction of Handshake pin, err: %d\n", status);
        return status;
    }

    status = request_irq(SPI_IRQ, spi_interrupt_handler,
                         IRQF_SHARED | IRQF_TRIGGER_RISING,
                         "ESP_SPI", spi_context.esp_spi_dev);

    if (status) {
        printk(KERN_ERR "Failed to request IRQ for Handshake pin, err:%d\n", status);
        return status;
    }

    status = gpio_request(SPI_DATA_READY_PIN, "SPI_DATA_READY_PIN");

    if (status) {
        printk(KERN_ERR "Failed to obtain GPIO for Data ready pin, err:%d\n", status);
        return status;
    }

    status = gpio_direction_input(SPI_DATA_READY_PIN);

    if (status) {
        printk(KERN_ERR "Failed to set GPIO direction of Data ready pin\n");
        return status;
    }

    status = request_irq(SPI_DATA_READY_IRQ, spi_data_ready_interrupt_handler,
                         IRQF_SHARED | IRQF_TRIGGER_RISING,
                         "ESP_SPI_DATA_READY", spi_context.esp_spi_dev);

    if (status) {
        printk(KERN_ERR "Failed to request IRQ for Data ready pin, err:%d\n", status);
        return status;
    }

    open_data_path();

    return 0;
}

static int spi_reinit_spidev(int spi_clk_mhz)
{
    disable_irq(SPI_IRQ);
    disable_irq(SPI_DATA_READY_IRQ);
    close_data_path();
    free_irq(SPI_IRQ, spi_context.esp_spi_dev);
    free_irq(SPI_DATA_READY_IRQ, spi_context.esp_spi_dev);
    gpio_free(HANDSHAKE_PIN);
    gpio_free(SPI_DATA_READY_PIN);

    if (spi_context.esp_spi_dev) {
        spi_unregister_device(spi_context.esp_spi_dev);
    }

    return spi_dev_init(spi_clk_mhz);
}

static int spi_init(void)
{
    int status = 0;
    uint8_t prio_q_idx = 0;

    spi_context.spi_workqueue = create_workqueue("ESP_SPI_WORK_QUEUE");

    if (!spi_context.spi_workqueue) {
        printk(KERN_ERR "spi workqueue failed to create\n");
        spi_exit();
        return -EFAULT;
    }

    INIT_WORK(&spi_context.spi_work, esp_spi_work);

    for (prio_q_idx = 0; prio_q_idx < MAX_PRIORITY_QUEUES; prio_q_idx++) {
        skb_queue_head_init(&spi_context.tx_q[prio_q_idx]);
        skb_queue_head_init(&spi_context.rx_q[prio_q_idx]);
    }


    status = spi_dev_init(SPI_INITIAL_CLK_MHZ);

    if (status) {
        spi_exit();
        printk(KERN_ERR "Failed Init SPI device\n");
        return status;
    }

#ifdef CONFIG_SUPPORT_ESP_SERIAL
    status = esp_serial_init((void *)spi_context.adapter);

    if (status != 0) {
        spi_exit();
        printk(KERN_ERR "Error initialising serial interface\n");
        return status;
    }

#endif

    status = esp_add_card(spi_context.adapter);

    if (status) {
        spi_exit();
        printk(KERN_ERR "Failed to add card\n");
        return status;
    }

    msleep(200);

    return status;
}

static void spi_exit(void)
{
    uint8_t prio_q_idx = 0;

    disable_irq(SPI_IRQ);
    disable_irq(SPI_DATA_READY_IRQ);
    close_data_path();
    msleep(200);

    for (prio_q_idx = 0; prio_q_idx < MAX_PRIORITY_QUEUES; prio_q_idx++) {
        skb_queue_purge(&spi_context.tx_q[prio_q_idx]);
        skb_queue_purge(&spi_context.rx_q[prio_q_idx]);
    }

    if (spi_context.spi_workqueue) {
        flush_scheduled_work();
        destroy_workqueue(spi_context.spi_workqueue);
        spi_context.spi_workqueue = NULL;
    }

    esp_serial_cleanup();
    esp_remove_card(spi_context.adapter);

    if (spi_context.adapter->hcidev) {
        esp_deinit_bt(spi_context.adapter);
    }

    free_irq(SPI_IRQ, spi_context.esp_spi_dev);
    free_irq(SPI_DATA_READY_IRQ, spi_context.esp_spi_dev);

    gpio_free(HANDSHAKE_PIN);
    gpio_free(SPI_DATA_READY_PIN);

    if (spi_context.esp_spi_dev) {
        spi_unregister_device(spi_context.esp_spi_dev);
    }

    memset(&spi_context, 0, sizeof(spi_context));
}

static void adjust_spi_clock(u8 spi_clk_mhz)
{
    if ((spi_clk_mhz) && (spi_clk_mhz != SPI_INITIAL_CLK_MHZ)) {
        printk(KERN_INFO "ESP Reconfigure SPI CLK to %u MHz\n", spi_clk_mhz);

        if (spi_reinit_spidev(spi_clk_mhz)) {
            printk(KERN_ERR "Failed to reinit SPI device\n");
            spi_exit();
            return;
        }
    }
}

int esp_init_interface_layer(struct esp_adapter *adapter)
{
    if (!adapter) {
        return -EINVAL;
    }

    memset(&spi_context, 0, sizeof(spi_context));

    adapter->if_context = &spi_context;
    adapter->if_ops = &if_ops;
    adapter->if_type = ESP_IF_TYPE_SPI;
    spi_context.adapter = adapter;

    return spi_init();
}

void esp_deinit_interface_layer(void)
{
    spi_exit();
}
