struct gui;
#define usb_callback_data_t struct gui

struct item;
#define usb_device_data_t struct item

#include <graphx.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tice.h>
#include <usbdrvce.h>
// #include <debug.h>
#include "driver.h"
#include <stdarg.h>
#include <sys/timers.h>
#include <lwip.h> //contains everything

#define CHAR_HEIGHT 8
#define SPACES "        "

#define DEBUG_LINES 25
#define DEBUG_LINE_LEN 64

#define CHECK_ERR(tag, err)                         \
    if ((err) != USB_SUCCESS)                       \
    {                                               \
        debug_log(item->gui, tag " ERROR=%d", err); \
    }                                               \
    else                                            \
    {                                               \
        debug_log(item->gui, tag " OK");            \
    }
static struct netif g_netif;
static bool g_lwip_ready = false;
enum
{
    background_color,
    text_color,
    selection_color,
    selected_text_color,
    transparent_color,
};

static const uint16_t palette[transparent_color] = {
    [background_color] = 0xFFFF,
    [text_color] = 0x0000,
    [selection_color] = 0x8D53,
    [selected_text_color] = 0xFFFF,
};

struct gui
{
    bool dirty;
    usb_device_t first_device;
    usb_device_t selected_device;
    int selected_y;
    bool scrolling;

    char debug_lines[DEBUG_LINES][DEBUG_LINE_LEN];
    int debug_index;
    int debug_scroll;
    int debug_count;
    int debug_top;
    int debug_total;
};

struct item
{
    struct gui *gui;
    uint8_t depth;
    bool collapsed;
    usb_control_setup_t setup;
    usb_device_descriptor_t device_descriptor;
    uint16_t langid;
    char manufacturer[0x80];
    char product[0x80];
    char serial_number[0x80];

    uint8_t bulk_in_address;
    uint8_t bulk_out_address;
    uint16_t bulk_in_max_packet;
    uint16_t bulk_out_max_packet;
    usb_endpoint_t bulk_in;
    usb_endpoint_t bulk_out;
    uint16_t phy_status;
    bool rx_running;
    uint16_t rx_header_size;
    uint16_t tx_header_size;

    bool is_ax88179;
    bool driver_initialized;
    ax88179_device_t axdev;
    union
    {
        usb_string_descriptor_t string_descriptor;
        usb_configuration_descriptor_t configuration_descriptor;
        uint8_t buffer[0xFF];
    };
};

void debug_log(struct gui *gui, const char *fmt, ...)
{
    if (!gui)
        return;

    va_list args;
    va_start(args, fmt);

    vsnprintf(gui->debug_lines[gui->debug_index], DEBUG_LINE_LEN, fmt, args);

    va_end(args);

    gui->debug_index = (gui->debug_index + 1) % DEBUG_LINES;

    if (gui->debug_count < DEBUG_LINES)
        gui->debug_count++;

    gui->debug_total++;

    /* Auto-follow newest */
    if (!gui->scrolling)
    {
        gui->debug_top = gui->debug_total - DEBUG_LINES;
        if (gui->debug_top < 0)
            gui->debug_top = 0;
    }

    gui->dirty = true;
}

void ax_log_wrapper(void *ctx, const char *fmt, ...)
{
    struct item *item = (struct item *)ctx;
    if (!item || !item->gui)
        return;

    struct gui *gui = item->gui;

    va_list args;
    va_start(args, fmt);

    vsnprintf(gui->debug_lines[gui->debug_index], DEBUG_LINE_LEN, fmt, args);

    va_end(args);

    gui->debug_index = (gui->debug_index + 1) % DEBUG_LINES;

    if (gui->debug_count < DEBUG_LINES)
        gui->debug_count++;

    gui->debug_total++;

    if (!gui->scrolling)
    {
        gui->debug_top = gui->debug_total - DEBUG_LINES;
        if (gui->debug_top < 0)
            gui->debug_top = 0;
    }

    gui->dirty = true;
}

static void network_init(struct gui *gui)
{
    debug_log(gui, "[LWIP] init...");

    lwip_init();

    ip4_addr_t ipaddr, netmask, gw;

    IP4_ADDR(&ipaddr, 0, 0, 0, 0); // DHCP
    IP4_ADDR(&netmask, 0, 0, 0, 0);
    IP4_ADDR(&gw, 0, 0, 0, 0);

    netif_add(&g_netif, &ipaddr, &netmask, &gw,
              NULL, // we'll attach later
              NULL, // driver init comes from USB side
              ethernet_input);

    netif_set_default(&g_netif);
    netif_set_up(&g_netif);

#if LWIP_DHCP
    dhcp_start(&g_netif);
    debug_log(gui, "[LWIP] DHCP started");
#endif

    g_lwip_ready = true;
}

static err_t tcp_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    struct gui *gui = (struct gui *)arg;

    if (err == ERR_OK)
    {
        debug_log(gui, "[TCP] Connected to .199 ✅");
    }
    else
    {
        debug_log(gui, "[TCP] Connect failed err=%d", err);
    }

    return ERR_OK;
}

static void try_connect_199(struct gui *gui)
{
    if (!g_lwip_ready)
        return;

    if (!netif_is_up(&g_netif))
    {
        debug_log(gui, "[TCP] netif not up yet");
        return;
    }

    ip4_addr_t target;
    ip4_addr_t *ip = netif_ip4_addr(&g_netif);

    if (ip4_addr_isany_val(*ip))
    {
        debug_log(gui, "[TCP] no IP yet");
        return;
    }

    // replace last octet with 199
    IP4_ADDR(&target,
             ip4_addr1(ip),
             ip4_addr2(ip),
             ip4_addr3(ip),
             199);

    struct tcp_pcb *pcb = tcp_new();
    if (!pcb)
    {
        debug_log(gui, "[TCP] pcb alloc failed");
        return;
    }

    debug_log(gui, "[TCP] connecting to %d.%d.%d.199",
              ip4_addr1(ip),
              ip4_addr2(ip),
              ip4_addr3(ip));

    tcp_arg(pcb, gui);
    tcp_connect(pcb, &target, 80, tcp_connected);
}

void outchar(char c)
{
    int x = gfx_GetTextX(), y = gfx_GetTextY(), width;
    if (c == '\n')
    {
        if (y >= 0 && y + CHAR_HEIGHT <= LCD_HEIGHT &&
            x >= 0 && x < LCD_WIDTH)
        {
            gfx_FillRectangle_NoClip(x, y, LCD_WIDTH - x, CHAR_HEIGHT);
        }
        gfx_SetTextXY(0, y + CHAR_HEIGHT);
        return;
    }
    if (c < ' ' || c > '~')
    {
        return;
    }
    width = gfx_GetCharWidth(c);
    if (y >= 0 && y + CHAR_HEIGHT <= LCD_HEIGHT &&
        x >= 0 && x + width <= LCD_WIDTH)
    {
        gfx_PrintChar(c);
        return;
    }
    if (y <= -CHAR_HEIGHT || y >= LCD_HEIGHT ||
        x <= -width || x >= LCD_WIDTH)
    {
        gfx_SetTextConfig(gfx_text_clip);
        gfx_PrintChar(c);
        gfx_SetTextConfig(gfx_text_noclip);
    }
}

static void set_colors(uint8_t bg, uint8_t fg)
{
    gfx_SetColor(bg);
    gfx_SetTextBGColor(bg);
    gfx_SetTextFGColor(fg);
}

static void set_device(usb_device_t *variable, usb_device_t value)
{
    usb_UnrefDevice(*variable);
    *variable = usb_RefDevice(value);
}

static void move_device(usb_device_t *variable, usb_device_t *value)
{
    usb_UnrefDevice(*variable);
    *variable = *value;
    *value = NULL;
}

static bool move_to_next_device(usb_device_t *device)
{
    usb_device_t next = NULL;
    struct item *item = usb_GetDeviceData(*device);
    usb_find_device_flags_t flags = USB_SKIP_DISABLED;
    if (item != NULL && item->collapsed)
    {
        flags |= USB_SKIP_ATTACHED;
    }
    set_device(&next, usb_FindDevice(NULL, *device, flags));
    if (next == NULL)
    {
        return false;
    }
    move_device(device, &next);
    return true;
}

static bool move_to_previous_device(usb_device_t *device)
{
    usb_device_t previous = NULL, current = NULL;
    set_device(&current, usb_RootHub());
    do
    {
        set_device(&previous, current);
        if (!move_to_next_device(&current))
        {
            return false;
        }
    } while (current != *device);
    move_device(device, &previous);
    set_device(&current, NULL);
    return true;
}

static void move_to_nearby_device(usb_device_t *device)
{
    if (!move_to_next_device(device) && !move_to_previous_device(device))
    {
        set_device(device, usb_RootHub());
    }
}

static void gui_draw(struct gui *gui)
{
    int y = 0;
    usb_device_t device = NULL;

    if (!gui->dirty)
    {
        return;
    }

    gfx_SetTextXY(0, y);
    set_colors(background_color, text_color);

    set_device(&device, gui->first_device);

    do
    {
        bool is_hub = usb_GetDeviceFlags(device) & USB_IS_HUB;
        struct item *item = usb_GetDeviceData(device);
        if (item == NULL)
        {
            continue;
        }

        if (device == gui->selected_device)
        {
            gui->selected_y = y;
            set_colors(selection_color, selected_text_color);
        }

        printf("%.*s%c%04X:%04X  %s %s %s\n",
               item->depth,
               SPACES,
               is_hub ? (item->collapsed ? '+' : '-') : '=',
               item->device_descriptor.idVendor,
               item->device_descriptor.idProduct,
               item->manufacturer,
               item->product,
               item->serial_number);

        printf("%.*s  IN:  %02X (%d)\n",
               item->depth,
               SPACES,
               item->bulk_in_address,
               item->bulk_in_max_packet);

        printf("%.*s  OUT: %02X (%d)\n",
               item->depth,
               SPACES,
               item->bulk_out_address,
               item->bulk_out_max_packet);

        if (device == gui->selected_device)
        {
            set_colors(background_color, text_color);
        }

        y = gfx_GetTextY();

    } while (y + CHAR_HEIGHT <= LCD_HEIGHT && move_to_next_device(&device));

    set_device(&device, NULL);

    /* Clear remaining screen */
    if (y >= 0 && y < LCD_HEIGHT)
    {
        gfx_FillRectangle_NoClip(0, y, LCD_WIDTH, LCD_HEIGHT - y);
    }

    /* ================= DEBUG OUTPUT ================= */
    printf("\n--- DEBUG ---\n");

    for (int i = 0; i < DEBUG_LINES; i++)
    {
        int logical = gui->debug_top + i;

        if (logical >= gui->debug_total)
            break;

        int idx = logical % DEBUG_LINES;

        const char *line = gui->debug_lines[idx];

        if (line[0])
            printf("%s\n", line);
    }

    gfx_SwapDraw();
    gui->dirty = false;
}

static struct item *create_item(struct gui *gui, usb_device_t device)
{
    struct item *parent = usb_GetDeviceData(usb_GetDeviceHub(device));
    struct item *item = malloc(sizeof(struct item));
    if (item == NULL)
    {
        return NULL;
    }
    item->gui = gui;
    item->depth = parent ? parent->depth + 1 : 0;
    item->collapsed = false;
    item->device_descriptor.bLength = 0;
    item->device_descriptor.bDescriptorType = 0;
    item->device_descriptor.idVendor = 0;
    item->device_descriptor.idProduct = 0;
    item->langid = 0;
    strcpy(item->manufacturer, "Unknown");
    strcpy(item->product, "Unknown");
    strcpy(item->serial_number, "Unknown");
    item->string_descriptor.bLength = 0;
    item->string_descriptor.bDescriptorType = 0;
    item->bulk_in_address = 0;
    item->bulk_out_address = 0;
    item->bulk_in_max_packet = 0;
    item->bulk_out_max_packet = 0;
    item->driver_initialized = false;
    item->axdev.log = ax_log_wrapper;
    item->axdev.log_ctx = item;
    return item;
}

static void usb_string_descriptor_to_ascii(const usb_string_descriptor_t *string, char *ascii)
{
    uint8_t index = 0, length = string->bLength;
    while (length >= 4)
    {
        wchar_t wc = string->bString[index++];
        if (wc == L'\0')
        {
            break;
        }
        if (wc < ' ' || wc > '~')
        {
            wc = '?';
        }
        *ascii++ = wc;
        length -= 2;
    }
    *ascii = '\0';
    return;
}

static usb_error_t get_configuration_descriptor_handler(usb_endpoint_t endpoint, usb_transfer_status_t status, size_t transferred, usb_transfer_data_t *data)
{
    usb_device_t device = usb_GetEndpointDevice(endpoint);
    struct item *item = usb_GetDeviceData(device);

    if (!item)
        return USB_SUCCESS;

    // debug_log(item->gui, "[CFG] Handler entered");

    if (status != USB_TRANSFER_COMPLETED || !data)
    {
        debug_log(item->gui, "[CFG] Invalid status=%d data=%p", status, data);
        return USB_SUCCESS;
    }

    ax88179_device_t *dev = &item->axdev;
    dev->device = device;

    const usb_configuration_descriptor_t *cfg = (const usb_configuration_descriptor_t *)data;

    // debug_log(item->gui, "[CFG] TotalLength=%d transferred=%d",              cfg->wTotalLength, transferred);

    uint8_t bulk_in_addr = 0;
    uint8_t bulk_out_addr = 0;
    const usb_interface_descriptor_t *chosen_iface = NULL;
    const usb_interface_descriptor_t *current_iface = NULL;
    uint8_t *ptr = (uint8_t *)cfg;
    uint8_t *end = ptr + cfg->wTotalLength;

    /* =========================       Parse descriptors
       ========================= */

    while (ptr < end)
    {
        uint8_t len = ptr[0];
        uint8_t type = ptr[1];

        if (len == 0)
            break;

        if (type == USB_INTERFACE_DESCRIPTOR)
        {
            const usb_interface_descriptor_t *iface = (const usb_interface_descriptor_t *)ptr;

            debug_log(item->gui, "[IFACE] class=0x%02X subclass=0x%02X proto=0x%02X", iface->bInterfaceClass, iface->bInterfaceSubClass, iface->bInterfaceProtocol);

            current_iface = (const usb_interface_descriptor_t *)ptr;

            if (current_iface->bInterfaceClass == USB_VENDOR_SPECIFIC_CLASS)
            {
                chosen_iface = current_iface;

                debug_log(item->gui, "[CFG] using vendor interface %d", current_iface->bInterfaceNumber);
            }
        }

        if (type == USB_ENDPOINT_DESCRIPTOR && current_iface == chosen_iface)
        {
            const usb_endpoint_descriptor_t *ep = (const usb_endpoint_descriptor_t *)ptr;

            uint8_t addr = ep->bEndpointAddress;
            uint8_t type_bits = ep->bmAttributes & 0x03;

            if (type_bits == USB_BULK_TRANSFER)
            {
                if ((addr & 0x80) && !bulk_in_addr)
                    bulk_in_addr = addr;

                if (!(addr & 0x80) && !bulk_out_addr)
                    bulk_out_addr = addr;
            }
        }

        ptr += len;
    }

    debug_log(item->gui, "[CFG] selected IN=0x%02X OUT=0x%02X", bulk_in_addr, bulk_out_addr);

    /* =========================       Apply configuration
       ========================= */
    if (usb_SetConfiguration(device, cfg, cfg->wTotalLength) != USB_SUCCESS)
    {
        debug_log(item->gui, "[CFG] ERROR: SetConfiguration failed");
        return USB_SUCCESS;
    }

    // debug_log(item->gui, "[CFG] Configuration applied");

    if (chosen_iface)
    {
        usb_SetInterface(device, chosen_iface, sizeof(*chosen_iface));
        // debug_log(item->gui,                  "[CFG] Interface %d activated",                  chosen_iface->bInterfaceNumber);
    }

    /* =========================       Validate endpoints
       ========================= */
    // debug_log(item->gui,              "[CFG] EP summary IN=0x%02X OUT=0x%02X",              bulk_in_addr, bulk_out_addr);

    if (!bulk_in_addr || !bulk_out_addr)
    {
        debug_log(item->gui, "[CFG] ERROR: missing endpoints");
        return USB_SUCCESS;
    }

    /* =========================       Attach endpoints
       ========================= */
    dev->ep_bulk_in = usb_GetDeviceEndpoint(device, bulk_in_addr);
    dev->ep_bulk_out = usb_GetDeviceEndpoint(device, bulk_out_addr);

    if (!dev->ep_bulk_in || !dev->ep_bulk_out)
    {
        debug_log(item->gui, "[CFG] ERROR: endpoint lookup failed");
        return USB_SUCCESS;
    }

    dev->max_packet_size = usb_GetEndpointMaxPacketSize(dev->ep_bulk_out);

    // debug_log(item->gui,              "[CFG] Endpoints ready maxpkt=%d",              dev->max_packet_size);

    /* =========================       Initialize AX88179
       ========================= */
    // debug_log(item->gui, "[CFG] Calling reset...");

    if (ax88179_reset(dev) != 0)
    {
        debug_log(item->gui, "[CFG] ERROR: reset failed");
        return USB_SUCCESS;
    }

    // debug_log(item->gui, "[CFG] Reset OK");

    item->driver_initialized = true;

    debug_log(item->gui, "[CFG] INIT DONE MAC=%02X:%02X:%02X:%02X:%02X:%02X", dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3], dev->mac[4], dev->mac[5]);

    dev->ip[0] = 192;
    dev->ip[1] = 168;
    dev->ip[2] = 222;
    dev->ip[3] = 200;
    debug_log(item->gui, "IP set to 192.168.222.200");
    usb_ScheduleBulkTransfer(dev->ep_bulk_in, dev->rx_buf, sizeof(dev->rx_buf), ax88179_rx_handler, dev);
    debug_log(item->gui, "[RX] started");
    debug_log(item->gui, "[TEST] rx size=%d", sizeof(dev->rx_buf));
    ax88179_test_send(dev);
    return USB_SUCCESS;
}

static usb_error_t get_total_length_handler(usb_endpoint_t endpoint, usb_transfer_status_t status, size_t transferred, usb_transfer_data_t *data)
{
    usb_device_t device = usb_GetEndpointDevice(endpoint);
    struct item *item = usb_GetDeviceData(device);
    usb_configuration_descriptor_t *configuration_descriptor;
    (void)data;
    if (item == NULL || status != USB_TRANSFER_COMPLETED ||
        transferred != 4 ||
        item->configuration_descriptor.bLength < transferred ||
        item->configuration_descriptor.bDescriptorType != USB_CONFIGURATION_DESCRIPTOR)
    {
        if (item && item->gui)
        {
            item->gui->dirty = true;
        }
        return USB_SUCCESS;
    }

    configuration_descriptor = malloc(item->configuration_descriptor.wTotalLength);
    if (configuration_descriptor == NULL)
    {
        if (item && item->gui)
        {
            item->gui->dirty = true;
        }
        return USB_SUCCESS;
    }
    item->setup.bmRequestType = USB_DEVICE_TO_HOST | USB_STANDARD_REQUEST | USB_RECIPIENT_DEVICE;
    item->setup.bRequest = USB_GET_DESCRIPTOR_REQUEST;
    item->setup.wValue = USB_CONFIGURATION_DESCRIPTOR << 8;
    item->setup.wIndex = 0;
    item->setup.wLength = item->configuration_descriptor.wTotalLength;
    return usb_ScheduleControlTransfer(endpoint, &item->setup, configuration_descriptor, &get_configuration_descriptor_handler, configuration_descriptor);
}

static usb_error_t get_serial_number_handler(usb_endpoint_t endpoint, usb_transfer_status_t status, size_t transferred, usb_transfer_data_t *data)
{
    usb_device_t device = usb_GetEndpointDevice(endpoint);
    struct item *item = usb_GetDeviceData(device);
    if (item == NULL || status != USB_TRANSFER_COMPLETED ||
        transferred < 2 || transferred > sizeof(item->buffer) ||
        item->string_descriptor.bLength < 2 || item->string_descriptor.bLength > transferred ||
        item->string_descriptor.bDescriptorType != USB_STRING_DESCRIPTOR)
    {
        if (item && item->gui)
        {
            item->gui->dirty = true;
        }
        return USB_SUCCESS;
    }
    usb_string_descriptor_to_ascii(&item->string_descriptor, item->serial_number);

    item->setup.bmRequestType = USB_DEVICE_TO_HOST | USB_STANDARD_REQUEST | USB_RECIPIENT_DEVICE;
    item->setup.bRequest = USB_GET_DESCRIPTOR_REQUEST;
    item->setup.wValue = USB_CONFIGURATION_DESCRIPTOR << 8;
    item->setup.wIndex = 0;
    item->setup.wLength = 4;
    return usb_ScheduleControlTransfer(endpoint, &item->setup, &item->configuration_descriptor, &get_total_length_handler, data);
}

static usb_error_t get_product_handler(usb_endpoint_t endpoint, usb_transfer_status_t status, size_t transferred, usb_transfer_data_t *data)
{
    usb_device_t device = usb_GetEndpointDevice(endpoint);
    struct item *item = usb_GetDeviceData(device);
    if (item == NULL || status != USB_TRANSFER_COMPLETED ||
        transferred < 2 || transferred > sizeof(item->buffer) ||
        item->string_descriptor.bLength < 2 || item->string_descriptor.bLength > transferred ||
        item->string_descriptor.bDescriptorType != USB_STRING_DESCRIPTOR)
    {
        if (item && item->gui)
        {
            item->gui->dirty = true;
        }
        return USB_SUCCESS;
    }
    usb_string_descriptor_to_ascii(&item->string_descriptor, item->product);

    if (item->device_descriptor.iSerialNumber == 0)
    {
        item->string_descriptor.bLength = sizeof(L"N/A");
        item->string_descriptor.bDescriptorType = USB_STRING_DESCRIPTOR;
        memcpy(item->string_descriptor.bString, L"N/A", sizeof(L"N/A") - sizeof(L'\0'));
        return get_serial_number_handler(endpoint, status, item->string_descriptor.bLength, data);
    }
    item->setup.bmRequestType = USB_DEVICE_TO_HOST | USB_STANDARD_REQUEST | USB_RECIPIENT_DEVICE;
    item->setup.bRequest = USB_GET_DESCRIPTOR_REQUEST;
    item->setup.wValue = USB_STRING_DESCRIPTOR << 8 | item->device_descriptor.iSerialNumber;
    item->setup.wIndex = item->langid;
    item->setup.wLength = sizeof(item->buffer);
    return usb_ScheduleControlTransfer(endpoint, &item->setup, &item->string_descriptor, &get_serial_number_handler, data);
}

static usb_error_t get_manufacturer_handler(usb_endpoint_t endpoint, usb_transfer_status_t status, size_t transferred, usb_transfer_data_t *data)
{
    usb_device_t device = usb_GetEndpointDevice(endpoint);
    struct item *item = usb_GetDeviceData(device);
    if (item == NULL || status != USB_TRANSFER_COMPLETED ||
        transferred < 2 || transferred > sizeof(item->buffer) ||
        item->string_descriptor.bLength < 2 || item->string_descriptor.bLength > transferred ||
        item->string_descriptor.bDescriptorType != USB_STRING_DESCRIPTOR)
    {
        if (item && item->gui)
        {
            item->gui->dirty = true;
        }
        return USB_SUCCESS;
    }
    usb_string_descriptor_to_ascii(&item->string_descriptor, item->manufacturer);

    if (item->device_descriptor.iProduct == 0)
    {
        item->string_descriptor.bLength = sizeof(L"N/A");
        item->string_descriptor.bDescriptorType = USB_STRING_DESCRIPTOR;
        memcpy(item->string_descriptor.bString, L"N/A", sizeof(L"N/A") - sizeof(L'\0'));
        return get_product_handler(endpoint, status, item->string_descriptor.bLength, data);
    }
    item->setup.bmRequestType = USB_DEVICE_TO_HOST | USB_STANDARD_REQUEST | USB_RECIPIENT_DEVICE;
    item->setup.bRequest = USB_GET_DESCRIPTOR_REQUEST;
    item->setup.wValue = USB_STRING_DESCRIPTOR << 8 | item->device_descriptor.iProduct;
    item->setup.wIndex = item->langid;
    item->setup.wLength = sizeof(item->buffer);
    return usb_ScheduleControlTransfer(endpoint, &item->setup, &item->string_descriptor, &get_product_handler, data);
}

static usb_error_t get_langid_handler(usb_endpoint_t endpoint, usb_transfer_status_t status, size_t transferred, usb_transfer_data_t *data)
{
    usb_device_t device = usb_GetEndpointDevice(endpoint);
    struct item *item = usb_GetDeviceData(device);
    if (status & USB_TRANSFER_STALLED)
    {
        status &= ~(USB_TRANSFER_FAILED | USB_TRANSFER_STALLED);
        transferred = 4;
        item->string_descriptor.bLength = 4;
        item->string_descriptor.bDescriptorType = USB_STRING_DESCRIPTOR;
        item->string_descriptor.bString[0] = 0x0409;
    }
    if (item == NULL || status != USB_TRANSFER_COMPLETED ||
        transferred != 4 ||
        item->string_descriptor.bLength < transferred ||
        item->string_descriptor.bDescriptorType != USB_STRING_DESCRIPTOR)
    {
        if (item && item->gui)
        {
            item->gui->dirty = true;
        }
        return USB_SUCCESS;
    }
    item->langid = item->string_descriptor.bString[0];

    if (item->device_descriptor.iManufacturer == 0)
    {
        item->string_descriptor.bLength = sizeof(L"N/A");
        item->string_descriptor.bDescriptorType = USB_STRING_DESCRIPTOR;
        memcpy(item->string_descriptor.bString, L"N/A", sizeof(L"N/A") - sizeof(L'\0'));
        return get_manufacturer_handler(endpoint, status, item->string_descriptor.bLength, data);
    }
    item->setup.bmRequestType = USB_DEVICE_TO_HOST | USB_STANDARD_REQUEST | USB_RECIPIENT_DEVICE;
    item->setup.bRequest = USB_GET_DESCRIPTOR_REQUEST;
    item->setup.wValue = USB_STRING_DESCRIPTOR << 8 | item->device_descriptor.iManufacturer;
    item->setup.wIndex = item->langid;
    item->setup.wLength = sizeof(item->buffer);
    return usb_ScheduleControlTransfer(endpoint, &item->setup, &item->string_descriptor, &get_manufacturer_handler, data);
}

static usb_error_t get_device_descriptor_handler(usb_endpoint_t endpoint, usb_transfer_status_t status, size_t transferred, usb_transfer_data_t *data)
{
    usb_device_t device = usb_GetEndpointDevice(endpoint);
    struct item *item = usb_GetDeviceData(device);
    if (item == NULL || status != USB_TRANSFER_COMPLETED ||
        transferred != sizeof(usb_device_descriptor_t) ||
        item->device_descriptor.bLength < transferred ||
        item->device_descriptor.bDescriptorType != USB_DEVICE_DESCRIPTOR)
    {
        if (item && item->gui)
        {
            item->gui->dirty = true;
        }
        return USB_SUCCESS;
    }

    item->setup.bmRequestType = USB_DEVICE_TO_HOST | USB_STANDARD_REQUEST | USB_RECIPIENT_DEVICE;
    item->setup.bRequest = USB_GET_DESCRIPTOR_REQUEST;
    item->setup.wValue = USB_STRING_DESCRIPTOR << 8;
    item->setup.wIndex = 0;
    item->setup.wLength = 4;

    if (item->device_descriptor.idVendor == 0x0B95 &&
        item->device_descriptor.idProduct == 0x1790)
    {
        item->is_ax88179 = true;
        debug_log(item->gui, "AX88179 detected");
    }

    return usb_ScheduleControlTransfer(endpoint, &item->setup, &item->string_descriptor, &get_langid_handler, data);
}

static usb_error_t enabled_handler(usb_device_t device)
{
    struct item *item = usb_GetDeviceData(device);
    if (item == NULL)
    {
        return USB_SUCCESS;
    }
    item->setup.bmRequestType = USB_DEVICE_TO_HOST | USB_STANDARD_REQUEST | USB_RECIPIENT_DEVICE;
    item->setup.bRequest = USB_GET_DESCRIPTOR_REQUEST;
    item->setup.wValue = USB_DEVICE_DESCRIPTOR << 8;
    item->setup.wIndex = 0;
    item->setup.wLength = sizeof(usb_device_descriptor_t);
    return usb_ScheduleDefaultControlTransfer(device, &item->setup, &item->device_descriptor, &get_device_descriptor_handler, NULL);
}

static usb_error_t event_handler(usb_event_t event, void *data, struct gui *gui)
{
    usb_error_t error = USB_SUCCESS;
    struct item *item = usb_GetDeviceData(data);

    switch (event)
    {
    default:
        break;

    case USB_DEVICE_DISCONNECTED_EVENT:
        if (gui->first_device == data)
        {
            move_to_nearby_device(&gui->first_device);
        }
        if (gui->selected_device == data)
        {
            move_to_nearby_device(&gui->selected_device);
        }
        usb_SetDeviceData(data, NULL);
        free(item);
        gui->dirty = true;
        break;

    case USB_DEVICE_CONNECTED_EVENT:
        item = create_item(gui, data);
        usb_SetDeviceData(data, item);

        if (!(usb_GetRole() & USB_ROLE_DEVICE))
        {
            usb_ResetDevice(data);
        }
        break;

    case USB_DEVICE_ENABLED_EVENT:
        item = usb_GetDeviceData(data);

        if (error == USB_SUCCESS)
        {
            error = enabled_handler(data);
        }
        break;
    }

    return error;
}

static void event_loop(struct gui *gui)
{
    gui->dirty = true;
    while (true)
    {
        bool is_hub_selected = usb_GetDeviceFlags(gui->selected_device) & USB_IS_HUB;
        struct item *selected_item = usb_GetDeviceData(gui->selected_device);
        switch (os_GetCSC())
        {
        case 0: /* Idle */
        {
            gui->scrolling = false;
            usb_HandleEvents();

            struct item *item = usb_GetDeviceData(gui->selected_device);

            if (item && item->driver_initialized)
            {
                ax88179_update_link(&item->axdev);
            }

            break;
        }
        case sk_Window:
        {
            if (gui->debug_top > 0)
                gui->debug_top--;

            gui->scrolling = true;
            gui->dirty = true;
            break;
        }

        case sk_Zoom:
        {
            int max_top = gui->debug_total - DEBUG_LINES;
            if (max_top < 0)
                max_top = 0;

            if (gui->debug_top < max_top)
                gui->debug_top++;

            gui->scrolling = true;
            gui->dirty = true;
            break;
        }

        case sk_Up: /* Move selection up */
            if (gui->selected_device == gui->first_device)
            {
                move_to_previous_device(&gui->first_device);
            }
            if (!move_to_previous_device(&gui->selected_device))
            {
                break;
            }
            gui->dirty = true;
            break;
        case sk_Down: /* Move selection down */
            if (!move_to_next_device(&gui->selected_device))
            {
                break;
            }
            if (gui->selected_y + CHAR_HEIGHT * 2 > LCD_HEIGHT)
            {
                move_to_next_device(&gui->first_device);
            }
            gui->dirty = true;
            break;
        case sk_Left: /* Collapse current selection */
            if (selected_item == NULL || !is_hub_selected || selected_item->collapsed)
            {
                break;
            }
            selected_item->collapsed = true;
            gui->dirty = true;
            break;
        case sk_Right: /* Expand current selection */
            if (selected_item == NULL || !is_hub_selected || !selected_item->collapsed)
            {
                break;
            }
            selected_item->collapsed = false;
            gui->dirty = true;
            break;
        case sk_Alpha:
        {
            debug_log(gui, "Alpha pressed");

            struct item *item = usb_GetDeviceData(gui->selected_device);

            if (item && item->driver_initialized)
            {
                ax88179_test_send(&item->axdev);
            }
            if (!item)
            {
                debug_log(gui, "NO ITEM");
            }
            else
            {
                debug_log(gui, "item->driver_initialized is %d", item->driver_initialized);
            }

            gui->dirty = true;
            break;
        }
        case sk_2nd:
        {
            struct item *item = usb_GetDeviceData(gui->selected_device);
            if (item && item->driver_initialized)
            {
                ax88179_check_link(&item->axdev);
            }
            else
            {
                if (!item)
                {
                    debug_log(gui, "NO ITEM");
                }
                else
                {
                    debug_log(gui, "item->driver_initialized is %d", item->driver_initialized);
                }
            }

            break;
        }
        case sk_Enter: /* Toggle collapsed state */
            if (selected_item == NULL || !is_hub_selected)
            {
                break;
            }
            selected_item->collapsed = !selected_item->collapsed;
            gui->dirty = true;
            break;
        case sk_Clear: /* Exit */
            return;
        }
        gui_draw(gui);
    }
}

int main()
{
    /* Initialize gui */
    static struct gui gui;
    gui.debug_scroll = 0;
    struct item *root_item = create_item(&gui, usb_RootHub());
    set_device(&gui.first_device, usb_RootHub());
    set_device(&gui.selected_device, usb_RootHub());
    strcpy(root_item->manufacturer, "TI");
    strcpy(root_item->product, "Root1 Hub");
    strcpy(root_item->serial_number, "N/A");

    /* Initialize usb */
    usb_Init(&event_handler, &gui, NULL, USB_DEFAULT_INIT_FLAGS);
    usb_SetDeviceData(usb_RootHub(), root_item);
    /* Initialize graphics drawing */
    gfx_Begin();
    gfx_SetDrawBuffer();
    gfx_SetPalette(palette, sizeof(palette), 0);
    gfx_SetTransparentColor(transparent_color);
    gfx_SetTextTransparentColor(transparent_color);

    /* Main loop */
    event_loop(&gui);

    /* End graphics drawing */
    gfx_End();

    /* Cleanup usb */
    usb_Cleanup();
    free(root_item);

    return 0;
}
