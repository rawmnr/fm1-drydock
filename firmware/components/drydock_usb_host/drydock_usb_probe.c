/* Electrical presence probe of the USB-C port, ported from fm1-flip's
 * src/transport/usb_bus.cpp (probe). The classification thresholds and the
 * weak pull-down trick are theirs; only the USJ/restore dance is trimmed to
 * our boot flow (the host takes the PHY right after we probe).
 */
#include "drydock_usb_probe.h"

#include <stddef.h>

#include "drydock_usb_host.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"

static const char *TAG = "dry_probe";

#define PIN_DM GPIO_NUM_19
#define PIN_DP GPIO_NUM_20
#define PIN_MASK ((1u << PIN_DM) | (1u << PIN_DP))

static drydock_bus_probe_t s_last;

static uint32_t iomux_reg(gpio_num_t pin)
{
    return pin == PIN_DP ? IO_MUX_GPIO20_REG : IO_MUX_GPIO19_REG;
}

/* GPIO side only: no drive, no peripheral routing, no pulls; input buffers on
 * so GPIO_IN reflects the pads. The pin function (USB pad) is unchanged. */
static void release_pins(void)
{
    REG_WRITE(GPIO_ENABLE_W1TC_REG, PIN_MASK);
    REG_WRITE(GPIO_OUT_W1TC_REG, PIN_MASK);
    const gpio_num_t pins[2] = {PIN_DM, PIN_DP};
    for (size_t i = 0; i < 2; i++) {
        esp_rom_gpio_connect_out_signal(pins[i], SIG_GPIO_OUT_IDX, false, false);
        gpio_pullup_dis(pins[i]);
        gpio_pulldown_dis(pins[i]);
        PIN_INPUT_ENABLE(iomux_reg(pins[i]));
    }
    REG_WRITE(GPIO_ENABLE_W1TC_REG, PIN_MASK);
}

const char *drydock_bus_presence_name(drydock_bus_presence_t p)
{
    switch (p) {
    case DRYDOCK_BUS_EMPTY: return "empty";
    case DRYDOCK_BUS_DEVICE: return "device";
    case DRYDOCK_BUS_UNSTABLE: return "unstable";
    case DRYDOCK_BUS_FAULT: return "fault";
    }
    return "?";
}

const char *drydock_bus_presence_hint(drydock_bus_presence_t p)
{
    switch (p) {
    case DRYDOCK_BUS_EMPTY:
        return "no pull-up: FM-1 must be ON (screen lit, charged battery) on a"
               " data USB-C cable (charge-only / active cables never work)";
    case DRYDOCK_BUS_DEVICE:
        return "device pull-up present: ready to enumerate";
    case DRYDOCK_BUS_UNSTABLE:
        return "the FM-1 drives its USB lines (left in download mode, or browning"
               " out): switch it OFF then ON, wait for its normal screen, retry";
    case DRYDOCK_BUS_FAULT:
        return "both lines high: try another cable / adapter (short or wiring fault)";
    }
    return "?";
}

drydock_bus_probe_t drydock_usb_bus_last_probe(void)
{
    return s_last;
}

esp_err_t drydock_usb_bus_probe(drydock_bus_probe_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Hand the PHY pads from the USB-Serial-JTAG to the GPIO matrix and switch
     * the stick's own 1.5 kOhm D+ pull-up off, so we only measure the peer. */
    usb_serial_jtag_ll_phy_enable_external(false); /* internal PHY -> USJ */
    usb_serial_jtag_pull_override_vals_t off = {0};
    usb_serial_jtag_ll_phy_enable_pull_override(&off);
    usb_serial_jtag_ll_phy_enable_pad(false);
    release_pins();
    gpio_pulldown_en(PIN_DM); /* ~45 kOhm: a device's 1.5 kOhm pull-up wins */
    gpio_pulldown_en(PIN_DP);
    vTaskDelay(pdMS_TO_TICKS(5));

    static const int N = 20000; /* ~100 ms at 5 us spacing */
    uint32_t count[4] = {0};
    uint32_t changes = 0;
    int last = -1;
    for (int i = 0; i < N; i++) {
        const uint32_t in = REG_READ(GPIO_IN_REG);
        const int s = ((in >> PIN_DP) & 1) | (((in >> PIN_DM) & 1) << 1);
        count[s]++;
        if (last >= 0 && s != last) {
            changes++;
        }
        last = s;
        esp_rom_delay_us(5);
    }
    gpio_pulldown_dis(PIN_DM);
    gpio_pulldown_dis(PIN_DP);

    const uint32_t one_pct = N / 100;
    const uint32_t most = N * 95 / 100;
    drydock_bus_presence_t p;
    if (count[3] > one_pct) {
        p = DRYDOCK_BUS_FAULT;
    } else if ((count[1] > one_pct && count[2] > one_pct) || changes > 200) {
        p = DRYDOCK_BUS_UNSTABLE;
    } else if (count[1] >= most || count[2] >= most) {
        p = DRYDOCK_BUS_DEVICE;
    } else if (count[0] >= most) {
        p = DRYDOCK_BUS_EMPTY;
    } else if (changes <= 2 && count[3] == 0) {
        /* a single clean edge: the device attached (or left) during the window */
        p = (last == 1 || last == 2) ? DRYDOCK_BUS_DEVICE : DRYDOCK_BUS_EMPTY;
    } else {
        p = DRYDOCK_BUS_UNSTABLE;
    }

    *out = (drydock_bus_probe_t){
        .presence = p,
        .se0 = count[0],
        .j = count[1],
        .k = count[2],
        .se1 = count[3],
        .changes = changes,
    };
    s_last = *out;
    /* the boot log dies with the console once the host owns the PHY: the
     * journal keeps the diagnosis readable on the USB Diag screen */
    drydock_usb_journal_add("probe %s (SE0=%lu J=%lu K=%lu)",
                            drydock_bus_presence_name(p),
                            (unsigned long)count[0], (unsigned long)count[1],
                            (unsigned long)count[2]);
    ESP_LOGI(TAG, "USB bus probe: %s (SE0=%lu J=%lu K=%lu SE1=%lu changes=%lu)",
             drydock_bus_presence_name(p), (unsigned long)count[0],
             (unsigned long)count[1], (unsigned long)count[2],
             (unsigned long)count[3], (unsigned long)changes);
    if (p != DRYDOCK_BUS_DEVICE) {
        ESP_LOGW(TAG, "%s", drydock_bus_presence_hint(p));
    }
    return ESP_OK;
}
