/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <dk_buttons_and_leds.h>
#include <esb.h>
#include <nrfx.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/types.h>
#if defined(CONFIG_CLOCK_CONTROL_NRF2)
#include <hal/nrf_lrcconf.h>
#endif
#if NRF54L_ERRATA_20_PRESENT
#include <hal/nrf_power.h>
#endif /* NRF54L_ERRATA_20_PRESENT */
#if defined(NRF54LM20A_ENGA_XXAA)
#include <hal/nrf_clock.h>
#endif /* defined(NRF54LM20A_ENGA_XXAA) */
#include <hal/nrf_radio.h>
#include <helpers/nrfx_gppi.h>
#include <nrfx_gpiote.h>

LOG_MODULE_REGISTER(esb_prx, CONFIG_ESB_PRX_APP_LOG_LEVEL);

static struct esb_payload rx_payload;
static struct esb_payload tx_payload = ESB_CREATE_PAYLOAD(0, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17);

static void leds_update(uint8_t value)
{
    uint32_t leds_mask = (!(value % 8 > 0 && value % 8 <= 4) ? DK_LED1_MSK : 0) |
                         (!(value % 8 > 1 && value % 8 <= 5) ? DK_LED2_MSK : 0) |
                         (!(value % 8 > 2 && value % 8 <= 6) ? DK_LED3_MSK : 0) | (!(value % 8 > 3) ? DK_LED4_MSK : 0);

    dk_set_leds(leds_mask);
}

void event_handler(struct esb_evt const *event)
{
    switch (event->evt_id)
    {
    case ESB_EVENT_TX_SUCCESS:
        LOG_DBG("TX SUCCESS EVENT");
        break;
    case ESB_EVENT_TX_FAILED:
        LOG_DBG("TX FAILED EVENT");
        break;
    case ESB_EVENT_RX_RECEIVED:
        int err;

        while ((err = esb_read_rx_payload(&rx_payload)) == 0)
        {
            LOG_DBG("Packet received, len %d : "
                    "0x%02x, 0x%02x, 0x%02x, 0x%02x, "
                    "0x%02x, 0x%02x, 0x%02x, 0x%02x",
                    rx_payload.length, rx_payload.data[0], rx_payload.data[1], rx_payload.data[2], rx_payload.data[3],
                    rx_payload.data[4], rx_payload.data[5], rx_payload.data[6], rx_payload.data[7]);

            leds_update(rx_payload.data[1]);
        }
        if (err && err != -ENODATA)
        {
            LOG_ERR("Error while reading rx packet");
        }
        break;
    }
}

#if defined(CONFIG_CLOCK_CONTROL_NRF)
int clocks_start(void)
{
    int err;
    int res;
    struct onoff_manager *clk_mgr;
    struct onoff_client clk_cli;

    clk_mgr = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
    if (!clk_mgr)
    {
        LOG_ERR("Unable to get the Clock manager");
        return -ENXIO;
    }

    sys_notify_init_spinwait(&clk_cli.notify);

    err = onoff_request(clk_mgr, &clk_cli);
    if (err < 0)
    {
        LOG_ERR("Clock request failed: %d", err);
        return err;
    }

    do
    {
        err = sys_notify_fetch_result(&clk_cli.notify, &res);
        if (!err && res)
        {
            LOG_ERR("Clock could not be started: %d", res);
            return res;
        }
    } while (err);

#if NRF54L_ERRATA_20_PRESENT
    if (nrf54l_errata_20())
    {
        nrf_power_task_trigger(NRF_POWER, NRF_POWER_TASK_CONSTLAT);
    }
#endif /* NRF54L_ERRATA_20_PRESENT */

#if defined(NRF54LM20A_ENGA_XXAA)
    /* MLTPAN-39 */
    nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_PLLSTART);
#endif

    LOG_DBG("HF clock started");
    return 0;
}

#elif defined(CONFIG_CLOCK_CONTROL_NRF2)

int clocks_start(void)
{
    int err;
    int res;
    const struct device *radio_clk_dev = DEVICE_DT_GET_OR_NULL(DT_CLOCKS_CTLR(DT_NODELABEL(radio)));
    struct onoff_client radio_cli;

    /** Keep radio domain powered all the time to reduce latency. */
    nrf_lrcconf_poweron_force_set(NRF_LRCCONF010, NRF_LRCCONF_POWER_DOMAIN_1, true);

    sys_notify_init_spinwait(&radio_cli.notify);

    err = nrf_clock_control_request(radio_clk_dev, NULL, &radio_cli);

    do
    {
        err = sys_notify_fetch_result(&radio_cli.notify, &res);
        if (!err && res)
        {
            LOG_ERR("Clock could not be started: %d", res);
            return res;
        }
    } while (err == -EAGAIN);

    /* HMPAN-84 */
    if (nrf54h_errata_84())
    {
        nrf_lrcconf_clock_always_run_force_set(NRF_LRCCONF000, 0, true);
        nrf_lrcconf_task_trigger(NRF_LRCCONF000, NRF_LRCCONF_TASK_CLKSTART_0);
    }

    LOG_DBG("HF clock started");

    return 0;
}

#else
BUILD_ASSERT(false, "No Clock Control driver");
#endif /* defined(CONFIG_CLOCK_CONTROL_NRF2) */

int esb_initialize(void)
{
    int err;
    /* These are arbitrary default addresses. In end user products
     * different addresses should be used for each set of devices.
     */
    uint8_t base_addr_0[4] = {0xE7, 0xE7, 0xE7, 0xE7};
    uint8_t base_addr_1[4] = {0xC2, 0xC2, 0xC2, 0xC2};
    uint8_t addr_prefix[8] = {0xE7, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8};

    struct esb_config config = ESB_DEFAULT_CONFIG;

    config.protocol = ESB_PROTOCOL_ESB_DPL;
    config.bitrate = ESB_BITRATE_2MBPS;
    config.mode = ESB_MODE_PRX;
    config.event_handler = event_handler;
    config.selective_auto_ack = true;
    if (IS_ENABLED(CONFIG_ESB_FAST_SWITCHING))
    {
        config.use_fast_ramp_up = true;
    }

    err = esb_init(&config);
    if (err)
    {
        return err;
    }

    err = esb_set_base_address_0(base_addr_0);
    if (err)
    {
        return err;
    }

    err = esb_set_base_address_1(base_addr_1);
    if (err)
    {
        return err;
    }

    err = esb_set_prefixes(addr_prefix, ARRAY_SIZE(addr_prefix));
    if (err)
    {
        return err;
    }

    return 0;
}

#define P1_11 (32 + 11)         // Port 1, pin 11
#define RADIO_MON_PIN (32 + 11) // P1.11 (connected on p1, unused), NRF_GPIO_PIN_MAP(1, 11)
static nrfx_gpiote_t gpiote_inst = NRFX_GPIOTE_INSTANCE(NRF_GPIOTE20);
// static nrfx_gppi_t gppi_inst;
static nrfx_gppi_handle_t gppi_ready_handle;
static nrfx_gppi_handle_t gppi_disabled_handle;

void radio_activity_mon(void)
{
    nrfx_err_t err;
    uint8_t gpiote_channel;

    if (!nrfx_gpiote_init_check(&gpiote_inst))
    {
        err = nrfx_gpiote_init(&gpiote_inst, NRFX_GPIOTE_DEFAULT_CONFIG_IRQ_PRIORITY);
        if (err != 0)
        {
            LOG_ERR("nrfx_gpiote_init failed: 0x%08X", err);
            return;
        }
    }
    err = nrfx_gpiote_channel_alloc(&gpiote_inst, &gpiote_channel);
    if (err != 0)
    {
        LOG_ERR("nrfx_gpiote_channel_alloc failed: 0x%08X", err);
        return;
    }
    const nrfx_gpiote_output_config_t out_cfg = {
        .drive = NRF_GPIO_PIN_S0S1,
        .input_connect = NRF_GPIO_PIN_INPUT_DISCONNECT,
        .pull = NRF_GPIO_PIN_NOPULL,
    };

    const nrfx_gpiote_task_config_t task_cfg = {
        .task_ch = gpiote_channel,
        .polarity = NRF_GPIOTE_POLARITY_TOGGLE,   /* Task pin, we will use SET/CLR tasks */
        .init_val = NRF_GPIOTE_INITIAL_VALUE_LOW, /* Start low */
    };
    err = nrfx_gpiote_output_configure(&gpiote_inst, RADIO_MON_PIN, &out_cfg, &task_cfg);
    if (err != 0)
    {
        LOG_ERR("nrfx_gpiote_output_configure failed: 0x%08X", err);
        return;
    }
    nrfx_gpiote_out_task_enable(&gpiote_inst, RADIO_MON_PIN);

    uint32_t eep_ready = nrf_radio_event_address_get(NRF_RADIO, NRF_RADIO_EVENT_READY);
    uint32_t eep_disabled = nrf_radio_event_address_get(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);

    uint32_t tep_set = nrfx_gpiote_set_task_address_get(&gpiote_inst, RADIO_MON_PIN);
    uint32_t tep_clr = nrfx_gpiote_clr_task_address_get(&gpiote_inst, RADIO_MON_PIN);
    /* SET/CLR task address helpers are provided by nrfx_gpiote.[[nrfx_gpiote
     * tasks](https://docs.nordicsemi.com/bundle/nrfx-apis-latest/page/group_nrfx_gpiote.html)] */

    //    nrfx_gppi_init(&gppi_inst); // mpsl probably calling this. hardfault if u do this.
    /* required once before using GPPI [[nrfx 4.0
                    GPPI](https://docs.nordicsemi.com/bundle/nrfx-apis-latest/page/md_doc_2nrfx_4_0_migration_guide.html#autotoc_md30)]
                  */

    /* READY -> SET (pin high) */
    err = nrfx_gppi_conn_alloc(eep_ready, tep_set, &gppi_ready_handle);
    if (err != 0)
    {
        LOG_ERR("nrfx_gppi_conn_alloc(READY) failed: 0x%08X", err);
        return;
    }

    /* DISABLED -> CLR (pin low) */
    err = nrfx_gppi_conn_alloc(eep_disabled, tep_clr, &gppi_disabled_handle);
    if (err != 0)
    {
        LOG_ERR("nrfx_gppi_conn_alloc(DISABLED) failed: 0x%08X", err);
        return;
    }

    nrfx_gppi_conn_enable(gppi_ready_handle);
    nrfx_gppi_conn_enable(gppi_disabled_handle);

    LOG_INF("Radio activity monitor on P1.11 initialized");
}

#include <debug/ppi_trace.h>
static void *radio_trace_handle;
static void radio_activity_mon_ppit(void)
{
    uint32_t evt_ready = nrf_radio_event_address_get(NRF_RADIO, NRF_RADIO_EVENT_READY);
    uint32_t evt_disabled = nrf_radio_event_address_get(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
    radio_trace_handle = ppi_trace_pair_config(RADIO_MON_PIN, evt_ready, evt_disabled);
    if (radio_trace_handle != NULL)
    {
        ppi_trace_enable(radio_trace_handle);
    }
}

int main(void)
{
    int err;

    LOG_INF("Enhanced ShockBurst prx sample");

#if defined(CONFIG_SOC_SERIES_NRF54HX)
    const struct device *dtm_uart = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_console));

    if (dtm_uart != NULL)
    {
        int ret = pm_device_runtime_get(dtm_uart);

        if (ret < 0)
        {
            printk("Failed to get DTM UART runtime PM: %d\n", ret);
        }
    }
#endif /* defined(CONFIG_SOC_SERIES_NRF54HX) */

    err = clocks_start();
    if (err)
    {
        return 0;
    }

    err = dk_leds_init();
    if (err)
    {
        LOG_ERR("LEDs initialization failed, err %d", err);
        return 0;
    }

    // radio_activity_mon();


    err = esb_initialize();
    if (err)
    {
        LOG_ERR("ESB initialization failed, err %d", err);
        return 0;
    }
	    radio_activity_mon_ppit();

    LOG_INF("Initialization complete");

    err = esb_write_payload(&tx_payload);
    if (err)
    {
        LOG_ERR("Write payload, err %d", err);
        return 0;
    }

    LOG_INF("Setting up for packet receiption");

    err = esb_start_rx();
    if (err)
    {
        LOG_ERR("RX setup failed, err %d", err);
        return 0;
    }

    /* return to idle thread */
    return 0;
}
