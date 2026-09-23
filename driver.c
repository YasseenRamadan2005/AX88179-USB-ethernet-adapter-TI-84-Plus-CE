#include <graphx.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tice.h>
#include <usbdrvce.h>
#include <debug.h>
#include "driver.h"
#include "defines.h"
#include <stdarg.h>

#define LE16(x) ((uint16_t)((((x) & 0xFF) << 8) | (((x) >> 8) & 0xFF)))

static const struct
{
    unsigned char ctrl, timer_l, timer_h, size, ifg;
} AX88179_BULKIN_SIZE[] = {
    {7, 0x4f, 0, 0x12, 0xff},
    {7, 0x20, 3, 0x16, 0xff},
    {7, 0xae, 7, 0x18, 0xff},
    {7, 0xcc, 0x4c, 0x18, 8},
};

/* =========================
   Logging
   ========================= */
static void dlog(ax88179_device_t *dev, const char *fmt, ...)
{
    if (!dev || !dev->log)
    {
        dbg_printf("[DLOG] invalid\n");
        return;
    }

    char buffer[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    dev->log(dev->log_ctx, buffer);
}

/* =========================
   USB CONTROL (FIXED)
   ========================= */
int ax88179_read_cmd(ax88179_device_t *dev, uint8_t cmd, uint16_t value, uint16_t index, uint16_t size, void *data)
{
    usb_control_setup_t setup;

    setup.bmRequestType = USB_DEVICE_TO_HOST | USB_VENDOR_REQUEST | USB_RECIPIENT_DEVICE;

    setup.bRequest = cmd;
    setup.wValue = value;
    setup.wIndex = index;
    setup.wLength = size;

    size_t transferred = 0;

    usb_error_t err = usb_DefaultControlTransfer(
        dev->device,
        &setup,
        data,
        USB_RETRY_FOREVER,
        &transferred);

    if (err != USB_SUCCESS || transferred != size)
    {
        if (err != USB_SUCCESS)
        {
            dlog(dev, "[READ ERR] %d", err);
            return -1;
        }

        dlog(dev, "READ: transferred [%d] != size [%d]", transferred, size);

        return -2;
    }
    return 0;
}

int ax88179_write_cmd(ax88179_device_t *dev, uint8_t cmd, uint16_t value, uint16_t index, uint16_t size, const void *data)
{
    usb_control_setup_t setup;

    setup.bmRequestType =
        USB_HOST_TO_DEVICE | USB_VENDOR_REQUEST | USB_RECIPIENT_DEVICE;

    setup.bRequest = cmd;
    setup.wValue = value;
    setup.wIndex = index;
    setup.wLength = size;

    size_t transferred = 0;

    usb_error_t err = usb_DefaultControlTransfer(
        dev->device,
        &setup,
        (void *)data,
        USB_RETRY_FOREVER,
        &transferred);

    if (err != USB_SUCCESS || transferred != size)
    {
        if (err != USB_SUCCESS)
            dlog(dev, "[WRITE ERR] %d", err);
        if (transferred != size)
            dlog(dev, "WRITE: transferred [%d] != size [%d]", transferred, size);
        return -1;
    }

    return 0;
}

int ax88179_update_link(ax88179_device_t *dev)
{
    uint16_t physr;

    if (ax88179_read_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID,
                         GMII_PHY_PHYSR, 2, &physr) < 0)
        return -1;

    if (!(physr & GMII_PHY_PHYSR_LINK))
    {
        if (dev->link_up)
        {
            dlog(dev, "[LINK] DOWN");
        }
        dev->link_up = 0;
        return 0;
    }

    /* Already up? do nothing */
    if (dev->link_up)
        return 0;

    dlog(dev, "[LINK] UP physr=0x%04X", physr);

    /* Reconfigure like Linux */
    uint8_t tmp[5];
    memcpy(tmp, &AX88179_BULKIN_SIZE[0], 5);

    ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RX_BULKIN_QCTRL, 5, 5, tmp);

    uint16_t mode =
        AX_MEDIUM_RECEIVE_EN |
        AX_MEDIUM_TXFLOW_CTRLEN |
        AX_MEDIUM_RXFLOW_CTRLEN;

    if ((physr & GMII_PHY_PHYSR_SMASK) == GMII_PHY_PHYSR_GIGA)
        mode |= AX_MEDIUM_GIGAMODE | AX_MEDIUM_EN_125MHZ;
    else if ((physr & GMII_PHY_PHYSR_SMASK) == GMII_PHY_PHYSR_100)
        mode |= AX_MEDIUM_PS;

    if (physr & GMII_PHY_PHYSR_FULL)
        mode |= AX_MEDIUM_FULL_DUPLEX;

    ax88179_write_cmd(dev, AX_ACCESS_MAC,
                      AX_MEDIUM_STATUS_MODE, 2, 2, &mode);

    dev->link_up = 1;

    return 0;
}

static usb_endpoint_t get_control_ep(usb_device_t dev)
{
    return usb_GetDeviceEndpoint(dev, 0);
}

int ax88179_get_mac_addr(ax88179_device_t *dev)
{
    int ret = ax88179_read_cmd(
        dev,
        AX_ACCESS_MAC,
        AX_NODE_ID,
        ETH_ALEN,
        ETH_ALEN,
        dev->mac);

    if (ret < 0)
    {
        dlog(dev, "Failed to read MAC");
        return ret;
    }

    dlog(dev,
         "RAW MAC READ: %02X:%02X:%02X:%02X:%02X:%02X",
         dev->mac[0], dev->mac[1], dev->mac[2],
         dev->mac[3], dev->mac[4], dev->mac[5]);

    return 0;
}

int ax88179_reset(ax88179_device_t *dev)
{
    dlog(dev, "=== NEW BUILD ===");
    uint8_t buf[5];
    uint16_t *tmp16 = (uint16_t *)buf;
    uint8_t *tmp = (uint8_t *)buf;

    /* =========================
       Power up PHY
       ========================= */
    *tmp16 = 0x0000;
    ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_PHYPWR_RSTCTL, 2, 2, tmp16);

    *tmp16 = AX_PHYPWR_RSTCTL_IPRL;
    ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_PHYPWR_RSTCTL, 2, 2, tmp16);

    delay(500);

    /* =========================
       Clock setup
       ========================= */
    *tmp = AX_CLK_SELECT_ACS | AX_CLK_SELECT_BCS;
    ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_CLK_SELECT, 1, 1, tmp);

    delay(200);

    /* =========================
       Read MAC address
       ========================= */
    ax88179_get_mac_addr(dev);
    dlog(dev,
         "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
         dev->mac[0], dev->mac[1], dev->mac[2],
         dev->mac[3], dev->mac[4], dev->mac[5]);

    /* =========================
       RX bulk configuration
       ========================= */
    memcpy(tmp, &AX88179_BULKIN_SIZE[0], 5);
    ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RX_BULKIN_QCTRL, 5, 5, tmp);

    dev->rx_urb_size = 1024 * 20;

    /* =========================
       Flow control
       ========================= */
    *tmp = 0x34;
    ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_PAUSE_WATERLVL_LOW, 1, 1, tmp);

    *tmp = 0x52;
    ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_PAUSE_WATERLVL_HIGH, 1, 1, tmp);

    /* =========================
       Enable checksum offload (optional but recommended)
       ========================= */
    *tmp = AX_RXCOE_IP | AX_RXCOE_TCP | AX_RXCOE_UDP |
           AX_RXCOE_TCPV6 | AX_RXCOE_UDPV6;

    ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RXCOE_CTL, 1, 1, tmp);

    *tmp = AX_TXCOE_IP | AX_TXCOE_TCP | AX_TXCOE_UDP |
           AX_TXCOE_TCPV6 | AX_TXCOE_UDPV6;

    ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_TXCOE_CTL, 1, 1, tmp);

    /* =========================
       Start RX engine
       ========================= */
    *tmp16 = AX_RX_CTL_DROPCRCERR |
             AX_RX_CTL_IPE |
             AX_RX_CTL_START |
             AX_RX_CTL_AP |
             AX_RX_CTL_AMALL |
             AX_RX_CTL_AB;

    ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_RX_CTL, 2, 2, tmp16);

    dev->rxctl = *tmp16;

    /* =========================
       Medium mode (Gigabit full duplex)
       ========================= */
    *tmp16 = AX_MEDIUM_RECEIVE_EN |
             AX_MEDIUM_TXFLOW_CTRLEN |
             AX_MEDIUM_RXFLOW_CTRLEN |
             AX_MEDIUM_FULL_DUPLEX |
             AX_MEDIUM_GIGAMODE;

    ax88179_write_cmd(dev, AX_ACCESS_MAC, AX_MEDIUM_STATUS_MODE, 2, 2, tmp16);

    return 0;
}

int ax88179_check_link(ax88179_device_t *dev)
{
    uint16_t physr = 0;
    int ret = ax88179_read_cmd(dev, AX_ACCESS_PHY, AX88179_PHY_ID, GMII_PHY_PHYSR, 2, &physr);

    if (ret < 0)
    {
        dlog(dev, "[ax88179_check_link] PHY read failed");
        return -1;
    }

    bool link = (physr & GMII_PHY_PHYSR_LINK) != 0;

    dev->link_up = link;

    if (!link)
        return 0;

    /* =========================
       Determine speed + duplex
       ========================= */
    uint16_t mode = AX_MEDIUM_RECEIVE_EN |
                    AX_MEDIUM_TXFLOW_CTRLEN |
                    AX_MEDIUM_RXFLOW_CTRLEN;

    switch (physr & GMII_PHY_PHYSR_SMASK)
    {
    case GMII_PHY_PHYSR_GIGA:
        mode |= AX_MEDIUM_GIGAMODE | AX_MEDIUM_EN_125MHZ;
        // dlog(dev, "Speed: 1000 Mbps");
        break;

    case GMII_PHY_PHYSR_100:
        mode |= AX_MEDIUM_PS;
        // dlog(dev, "Speed: 100 Mbps");
        break;

    default:
        // dlog(dev, "Speed: 10 Mbps");
        break;
    }

    if (physr & GMII_PHY_PHYSR_FULL)
    {
        mode |= AX_MEDIUM_FULL_DUPLEX;
        // dlog(dev, "Duplex: FULL");
    }
    else
    {
        // dlog(dev, "Duplex: HALF");
    }

    /* =========================
       Apply medium mode
       ========================= */
    ax88179_write_cmd(
        dev,
        AX_ACCESS_MAC,
        AX_MEDIUM_STATUS_MODE,
        2,
        2,
        &mode);

    return 0;
}

int ax88179_send(ax88179_device_t *dev, uint8_t *data, int len)
{
    uint8_t buffer[2048];

    uint32_t *hdr = (uint32_t *)buffer;

    uint32_t tx_hdr1 = len;
    uint32_t tx_hdr2 = 0;

    /* alignment rule from Linux driver */
    if (((len + 8) % dev->max_packet_size) == 0)
        tx_hdr2 |= 0x80008000;

    hdr[0] = tx_hdr1;
    hdr[1] = tx_hdr2;

    memcpy(buffer + 8, data, len);

    int total = len + 8;

    size_t transferred;

    usb_error_t err = usb_BulkTransfer(dev->ep_bulk_out, buffer, total, USB_RETRY_FOREVER, &transferred);

    if (err != USB_SUCCESS)
    {
        dlog(dev, "TX failed err=%d", err);
        return -1;
    }

    dlog(dev, "TX %d bytes", len);

    return 0;
}

int ax88179_tx_fixup(ax88179_device_t *dev, const uint8_t *packet, size_t packet_len, uint8_t *out_buf, size_t *out_len)
{
    uint32_t tx_hdr1;
    uint32_t tx_hdr2 = 0;
    size_t frame_size = dev->max_packet_size;

    if (packet_len == 0 || !packet || !out_buf || !out_len)
        return -1;

    /* =========================
       Build header
       ========================= */

    tx_hdr1 = (uint32_t)packet_len;

    /* Padding rule from original driver */
    if (((packet_len + 8) % frame_size) == 0)
    {
        tx_hdr2 |= 0x80008000;
    }

    /* =========================
       Write header (little-endian)
       ========================= */

    out_buf[0] = (uint8_t)(tx_hdr1 & 0xFF);
    out_buf[1] = (uint8_t)((tx_hdr1 >> 8) & 0xFF);
    out_buf[2] = (uint8_t)((tx_hdr1 >> 16) & 0xFF);
    out_buf[3] = (uint8_t)((tx_hdr1 >> 24) & 0xFF);

    out_buf[4] = (uint8_t)(tx_hdr2 & 0xFF);
    out_buf[5] = (uint8_t)((tx_hdr2 >> 8) & 0xFF);
    out_buf[6] = (uint8_t)((tx_hdr2 >> 16) & 0xFF);
    out_buf[7] = (uint8_t)((tx_hdr2 >> 24) & 0xFF);

    /* =========================
       Copy packet data
       ========================= */

    memcpy(out_buf + 8, packet, packet_len);

    *out_len = packet_len + 8;

    return 0;
}

void ax88179_test_send(ax88179_device_t *dev)
{
    uint8_t pkt[64] = {0};

    /* =========================
       Ethernet header
       ========================= */
    memset(pkt, 0xff, 6);         // dest MAC (broadcast)
    memcpy(pkt + 6, dev->mac, 6); // source MAC
    pkt[12] = 0x08;               // EtherType = ARP
    pkt[13] = 0x06;

    /* =========================
       ARP packet
       ========================= */
    pkt[14] = 0x00;
    pkt[15] = 0x01; // hardware type Ethernet
    pkt[16] = 0x08;
    pkt[17] = 0x00; // protocol IPv4
    pkt[18] = 6;    // MAC length
    pkt[19] = 4;    // IP length
    pkt[20] = 0x00;
    pkt[21] = 0x01; // opcode = request

    memcpy(pkt + 22, dev->mac, 6); // sender MAC

    pkt[28] = 192;
    pkt[29] = 168;
    pkt[30] = 222;
    pkt[31] = 123; // fake sender IP

    memset(pkt + 32, 0x00, 6); // target MAC

    pkt[38] = 192;
    pkt[39] = 168;
    pkt[40] = 222;
    pkt[41] = 1; // router IP

    int len = 42;

    /* Pad to minimum 60 bytes */
    if (len < 60)
        len = 60;

    ax88179_send(dev, pkt, len);
}

usb_error_t ax88179_rx_handler(usb_endpoint_t ep, usb_transfer_status_t status, size_t transferred, usb_transfer_data_t *data)
{
    ax88179_device_t *dev = (ax88179_device_t *)data;

    if (!dev)
        return USB_SUCCESS;

    if (status != USB_TRANSFER_COMPLETED || transferred < 16)
        goto reschedule;

    uint8_t *buf = dev->rx_buf;
    uint8_t *end = buf + transferred;

    uint8_t *packet = buf + 2;

    if (packet + 14 > end)
        goto reschedule;

    uint16_t eth = (packet[12] << 8) | packet[13];

    if (eth == 0x0806)
        handle_arp(dev, packet, end - packet);

    else if (eth == 0x0800)
        handle_ipv4(dev, packet, end - packet);

reschedule:
    usb_ScheduleBulkTransfer(
        dev->ep_bulk_in,
        dev->rx_buf,
        sizeof(dev->rx_buf),
        ax88179_rx_handler,
        dev);

    return USB_SUCCESS;
}

int ax88179_recv(ax88179_device_t *dev, void *out_buf, size_t max_len)
{
    uint8_t tmp[512];

    size_t transferred = 0;

    // dlog(dev, "[RX] poll");

    usb_error_t err = usb_BulkTransfer(
        dev->ep_bulk_in,
        tmp,
        sizeof(tmp),
        1, // minimal retries (NOT timeout)
        &transferred);

    /* Treat timeout + no data as normal */
    if (err == USB_ERROR_TIMEOUT || transferred == 0)
        return 0;

    if (err != USB_SUCCESS)
    {
        dlog(dev, "[RX] err=%d", err);
        return -1;
    }

    /* Sanity check */
    if (transferred < 8)
        return 0;

    /* AX88179 puts length near end (USB framing) */
    uint16_t pkt_len;
    memcpy(&pkt_len, tmp + transferred - 6, 2);
    pkt_len &= 0x1FFF;

    if (pkt_len == 0 || pkt_len > max_len)
    {
        dlog(dev, "[RX] bad length=%d", pkt_len);
        return 0;
    }

    /* Safety: ensure payload actually exists in buffer */
    if ((pkt_len + 2) > transferred)
    {
        dlog(dev, "[RX] truncated packet len=%d xfer=%d", pkt_len, transferred);
        return 0;
    }

    memcpy(out_buf, tmp + 2, pkt_len);

    /* Only log occasionally to avoid flooding */
    static int rx_log_div = 0;
    rx_log_div++;

    if ((rx_log_div % 10) == 0)
    {
        dlog(dev, "[RX] ok len=%d", pkt_len);
    }

    return pkt_len;
}

void handle_arp(ax88179_device_t *dev, uint8_t *pkt, size_t len)
{
    if (len < 42)
        return;

    uint8_t *arp = pkt + 14;

    uint16_t op = (arp[6] << 8) | arp[7];

    dlog(dev, "[ARP] op=%d", op);

    /* Only handle ARP request */
    if (op != 1)
        return;

    uint8_t *sender_mac = arp + 8;
    uint8_t *sender_ip = arp + 14;
    uint8_t *target_ip = arp + 24;

    dlog(dev,
         "[ARP] who-has %d.%d.%d.%d?",
         target_ip[0], target_ip[1],
         target_ip[2], target_ip[3]);

    /* Check if it’s asking for us */
    if (memcmp(target_ip, dev->ip, 4) != 0)
    {
        dlog(dev, "[ARP] not for us. %d.%d.%d.%d", dev->ip[0], dev->ip[1], dev->ip[2], dev->ip[3]);
        return;
    }

    dlog(dev, "[ARP] request is for us → replying");

    /* =========================
       Build ARP reply
       ========================= */
    uint8_t reply[60] = {0};

    /* Ethernet header */
    memcpy(reply, sender_mac, 6);   // dest = requestor
    memcpy(reply + 6, dev->mac, 6); // src = us
    reply[12] = 0x08;
    reply[13] = 0x06;

    uint8_t *rarp = reply + 14;

    /* ARP header */
    rarp[0] = 0x00;
    rarp[1] = 0x01; // Ethernet
    rarp[2] = 0x08;
    rarp[3] = 0x00; // IPv4
    rarp[4] = 6;    // MAC len
    rarp[5] = 4;    // IP len
    rarp[6] = 0x00;
    rarp[7] = 0x02; // Reply

    memcpy(rarp + 8, dev->mac, 6);    // sender MAC = us
    memcpy(rarp + 14, dev->ip, 4);    // sender IP  = us
    memcpy(rarp + 18, sender_mac, 6); // target MAC
    memcpy(rarp + 24, sender_ip, 4);  // target IP

    /* Send */
    ax88179_send(dev, reply, 60);

    dlog(dev, "[ARP] reply sent");
}

void handle_ipv4(ax88179_device_t *dev, uint8_t *pkt, size_t len)
{
    if (len < 34)
        return;

    uint8_t *ip = pkt + 14;

    uint8_t proto = ip[9];

    // Only ICMP
    if (proto != 1)
        return;

    uint8_t *src_ip = ip + 12;
    uint8_t *dst_ip = ip + 16;

    dlog(dev, "[IP] proto=%d dst=%d.%d.%d.%d", proto, dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3]);

    if (memcmp(dst_ip, dev->ip, 4) != 0)
    {
        dlog(dev, "[IP] not for us");
        return;
    }

    handle_icmp(dev, pkt, len);
}

uint16_t checksum16(uint8_t *buf, int len)
{
    uint32_t sum = 0;

    for (int i = 0; i < len; i += 2)
    {
        uint16_t word = buf[i] << 8;
        if (i + 1 < len)
            word |= buf[i + 1];

        sum += word;
    }

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return ~sum;
}

void handle_icmp(ax88179_device_t *dev, uint8_t *pkt, size_t len)
{
    uint8_t *ip = pkt + 14;
    uint8_t *icmp = ip + 20;

    if (len < 42)
        return;

    uint8_t type = icmp[0];

    dlog(dev, "[ICMP] type=%d", type);

    // Echo request only
    if (type != 8)
        return;

    dlog(dev, "[ICMP] echo request → reply");

    static uint8_t reply[1500];

    int ip_len = (ip[0] & 0x0F) * 4;
    int total_len = (ip[2] << 8) | ip[3];
    int icmp_len = total_len - ip_len;

    /* =========================
       Ethernet
       ========================= */
    memcpy(reply, pkt + 6, 6); // dest = sender
    memcpy(reply + 6, dev->mac, 6);
    reply[12] = 0x08;
    reply[13] = 0x00;

    uint8_t *rip = reply + 14;

    /* =========================
       IP header
       ========================= */
    memcpy(rip, ip, ip_len);

    // swap IPs
    memcpy(rip + 12, dev->ip, 4);
    memcpy(rip + 16, ip + 12, 4);

    rip[8] = 64; // TTL

    // fix checksum
    rip[10] = 0;
    rip[11] = 0;
    uint16_t ip_sum = checksum16(rip, ip_len);
    rip[10] = ip_sum >> 8;
    rip[11] = ip_sum & 0xFF;

    uint8_t *ricmp = rip + ip_len;

    /* =========================
       ICMP
       ========================= */
    memcpy(ricmp, icmp, icmp_len);

    ricmp[0] = 0; // Echo reply

    ricmp[2] = 0;
    ricmp[3] = 0;

    uint16_t sum = checksum16(ricmp, icmp_len);
    ricmp[2] = sum >> 8;
    ricmp[3] = sum & 0xFF;

    int total = 14 + ip_len + icmp_len;

    ax88179_send(dev, reply, total);

    dlog(dev, "[ICMP] reply sent");
}

int ax88179_attach(ax88179_device_t *dev, usb_device_t usb_dev)
{
    memset(dev, 0, sizeof(*dev));

    dev->device = usb_dev;
    dev->ep_control = get_control_ep(usb_dev);

    dlog(dev, "attach OK ep=%p", dev->ep_control);

    return 0;
}

int ax88179_init(ax88179_device_t *dev)
{
    dlog(dev, "[INIT] begin");

    size_t total = usb_GetConfigurationDescriptorTotalLength(dev->device, 0);
    dlog(dev, "[INIT] total config len=%u", (unsigned)total);

    if (!total)
    {
        dlog(dev, "[INIT] ERROR: total length is 0");
        return -1;
    }

    uint8_t *cfg = malloc(total);
    dlog(dev, "[INIT] malloc cfg=%p", cfg);

    if (!cfg)
    {
        dlog(dev, "[INIT] ERROR: malloc failed");
        return -1;
    }

    size_t len = 0;

    usb_error_t err = usb_GetConfigurationDescriptor(dev->device, 0, cfg, total, &len);

    dlog(dev, "[INIT] get cfg err=%d len=%u", err, (unsigned)len);

    if (err != USB_SUCCESS)
    {
        dlog(dev, "[INIT] ERROR: get descriptor failed");
        free(cfg);
        return -1;
    }

    err = usb_SetConfiguration(dev->device, (usb_configuration_descriptor_t *)cfg, total);

    dlog(dev, "[INIT] set config err=%d", err);

    if (err != USB_SUCCESS)
    {
        dlog(dev, "[INIT] ERROR: set configuration failed");
        free(cfg);
        return -1;
    }

    free(cfg);

    dlog(dev, "[INIT] calling reset");

    if (ax88179_reset(dev) != 0)
    {
        dlog(dev, "[INIT] ERROR: reset failed");
        return -1;
    }

    dlog(dev, "[INIT] reset OK");

    ax88179_check_link(dev);

    dlog(dev, "[INIT] done");

    return 0;
}