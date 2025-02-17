/*
 * Copyright (c) 2018 - 2022, Nordic Semiconductor ASA
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <nrfx.h>

#if NRFX_CHECK(NRFX_NFCT_ENABLED)

#include <nrfx_nfct.h>

#define NRFX_LOG_MODULE NFCT
#include <nrfx_log.h>

#if !defined(USE_WORKAROUND_FOR_ANOMALY_79) &&          \
    (defined(NRF52832_XXAA) || defined(NRF52832_XXAB))
#define USE_WORKAROUND_FOR_ANOMALY_79 1
#endif

#if !defined(USE_WORKAROUND_FOR_ANOMALY_190) &&          \
    (defined(NRF52833_XXAA) || defined(NRF52840_XXAA) || \
     defined(NRF5340_XXAA_APPLICATION))
#define USE_WORKAROUND_FOR_ANOMALY_190 1
#endif

#if NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_79) || NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_190)
#define NFCT_WORKAROUND_USES_TIMER 1
#endif

#if NRFX_CHECK(NFCT_WORKAROUND_USES_TIMER)
#include <nrfx_timer.h>

typedef struct
{
    const nrfx_timer_t timer;                     /**< Timer instance that supports the correct NFC field detection. */
#if NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_190)
    bool               fieldevents_filter_active; /**< Flag that indicates that the field events are ignored. */
    bool               is_hfclk_on;               /**< HFCLK has started - one of the NFC activation conditions. */
    bool               is_delayed;                /**< Required time delay has passed - one of the NFC activation conditions. */
#elif NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_79)
    uint32_t           field_state_cnt;           /**< Counter of the FIELDLOST events. */
#endif // NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_190)
} nrfx_nfct_timer_workaround_t;

#if NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_190)
    #define NRFX_NFCT_ACTIVATE_DELAY     1000 /**< Minimal delay in us between NFC field detection and activation of NFCT. */
    #define NRFX_NFCT_TIMER_PERIOD       NRFX_NFCT_ACTIVATE_DELAY
#elif NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_79)
    #define NRFX_NFCT_FIELDLOST_THR      7
    #define NRFX_NFCT_FIELD_TIMER_PERIOD 100  /**< Field polling period in us. */
    #define NRFX_NFCT_TIMER_PERIOD       NRFX_NFCT_FIELD_TIMER_PERIOD
#endif // NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_190)

static nrfx_nfct_timer_workaround_t m_timer_workaround =
{
    .timer = NRFX_TIMER_INSTANCE(NRFX_NFCT_CONFIG_TIMER_INSTANCE_ID),
};
#endif // NRFX_CHECK(NFCT_WORKAROUND_USES_TIMER)

#define NFCT_FRAMEDELAYMAX_DEFAULT     (0x00001000UL) /**< Default value of the FRAMEDELAYMAX. */
#define NFCT_FRAMEDELAYMIN_DEFAULT     (0x00000480UL) /**< Default value of the FRAMEDELAYMIN. */

/* Mask of all possible interrupts that are relevant for data reception. */
#define NRFX_NFCT_RX_INT_MASK (NRF_NFCT_INT_RXFRAMESTART_MASK | \
                               NRF_NFCT_INT_RXFRAMEEND_MASK   | \
                               NRF_NFCT_INT_RXERROR_MASK)

/* Mask of all possible interrupts that are relevant for data transmission. */
#define NRFX_NFCT_TX_INT_MASK (NRF_NFCT_INT_TXFRAMESTART_MASK | \
                               NRF_NFCT_INT_TXFRAMEEND_MASK)


/* Mask of all possible errors from the @ref NRF_NFCT_EVENT_RXERROR event. */
#define NRFX_NFCT_FRAME_STATUS_RX_ALL_MASK (NRF_NFCT_RX_FRAME_STATUS_CRC_MASK    | \
                                            NRF_NFCT_RX_FRAME_STATUS_PARITY_MASK | \
                                            NRF_NFCT_RX_FRAME_STATUS_OVERRUN_MASK)

/* Mask of all possible errors from the @ref NRF_NFCT_EVENT_ERROR event. */
#if defined (NRF52832_XXAA) || defined(NRF52832_XXAB)
#define NRFX_NFCT_ERROR_STATUS_ALL_MASK (NRF_NFCT_ERROR_FRAMEDELAYTIMEOUT_MASK | \
                                         NRF_NFCT_ERROR_NFCFIELDTOOSTRONG_MASK | \
                                         NRF_NFCT_ERROR_NFCFIELDTOOWEAK_MASK)
#else
#define NRFX_NFCT_ERROR_STATUS_ALL_MASK (NRF_NFCT_ERROR_FRAMEDELAYTIMEOUT_MASK)
#endif

/* Macros for conversion of bits to bytes. */
#define NRFX_NFCT_BYTES_TO_BITS(_bytes) ((_bytes) << 3)
#define NRFX_NFCT_BITS_TO_BYTES(_bits)  ((_bits)  >> 3)

/* Macro for checking whether the NFCT interrupt is active. */
#define NRFX_NFCT_EVT_ACTIVE(_name) \
    (nrf_nfct_event_check(NRF_NFCT, NRFX_CONCAT_2(NRF_NFCT_EVENT_, _name)) && \
     nrf_nfct_int_enable_check(NRF_NFCT, NRFX_CONCAT_3(NRF_NFCT_INT_, _name, _MASK)))

/* Macro for callback execution. */
#define NRFX_NFCT_CB_HANDLE(_cb, _evt) \
    if (_cb != NULL)                   \
    {                                  \
        _cb(&_evt);                    \
    }

typedef enum
{
    NRFX_NFC_FIELD_STATE_NONE,   /**< Initial value that indicates no NFCT field events. */
    NRFX_NFC_FIELD_STATE_OFF,    /**< The NFCT FIELDLOST event has been set. */
    NRFX_NFC_FIELD_STATE_ON,     /**< The NFCT FIELDDETECTED event has been set. */
    NRFX_NFC_FIELD_STATE_UNKNOWN /**< Both NFCT field events have been set - ambiguous state. */
} nrfx_nfct_field_state_t;

/**@brief NFCT control block. */
typedef struct
{
    nrfx_nfct_config_t config;
    nrfx_drv_state_t   state;
    volatile bool      field_on;
    uint32_t           frame_delay_max;
    uint32_t           frame_delay_min;
} nrfx_nfct_control_block_t;

static nrfx_nfct_control_block_t m_nfct_cb;

/**
 * @brief Common part of the setup used for the NFCT initialization and reinitialization.
 */
static void nrfx_nfct_hw_init_setup(void)
{
    // Use Window Grid frame delay mode.
    nrf_nfct_frame_delay_mode_set(NRF_NFCT, NRF_NFCT_FRAME_DELAY_MODE_WINDOWGRID);

    /* Begin: Workaround for anomaly 25 */
    /* Workaround for wrong SENSRES values require using SDD00001, but here SDD00100 is used
       because it is required to operate with Windows Phone */
    nrf_nfct_sensres_bit_frame_sdd_set(NRF_NFCT, NRF_NFCT_SENSRES_BIT_FRAME_SDD_00100);
    /* End: Workaround for anomaly 25 */
}

static void nrfx_nfct_frame_delay_max_set(bool default_delay)
{
    if (default_delay)
    {
        nrf_nfct_frame_delay_max_set(NRF_NFCT, NFCT_FRAMEDELAYMAX_DEFAULT);
    }
    else
    {
        nrf_nfct_frame_delay_max_set(NRF_NFCT, m_nfct_cb.frame_delay_max);
    }
}

/**@brief Function for evaluating and handling the NFC field events.
 *
 * @param[in]  field_state  Current field state.
 */
static void nrfx_nfct_field_event_handler(volatile nrfx_nfct_field_state_t field_state)
{
    nrfx_nfct_evt_t nfct_evt;

#if NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_190)
    if(m_timer_workaround.fieldevents_filter_active)
    {
        return;
    }
#endif // NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_190)

    if (field_state == NRFX_NFC_FIELD_STATE_UNKNOWN)
    {
        /* Probe NFC field */
        field_state = (nrfx_nfct_field_check()) ? NRFX_NFC_FIELD_STATE_ON :
                                                  NRFX_NFC_FIELD_STATE_OFF;
    }

    /* Field event service. Only take action on field transition -
     * based on the value of m_nfct_cb.field_on
     */
    switch (field_state)
    {
        case NRFX_NFC_FIELD_STATE_ON:
            if (!m_nfct_cb.field_on)
            {
#if NRFX_CHECK(NFCT_WORKAROUND_USES_TIMER)
#if NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_190)
                m_timer_workaround.is_hfclk_on               = false;
                m_timer_workaround.is_delayed                = false;
                m_timer_workaround.fieldevents_filter_active = true;

                nrfx_timer_clear(&m_timer_workaround.timer);
                nrfx_timer_enable(&m_timer_workaround.timer);
#elif NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_79)
                nrfx_timer_clear(&m_timer_workaround.timer);
                nrfx_timer_enable(&m_timer_workaround.timer);
                m_timer_workaround.field_state_cnt = 0;
#endif // NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_190)
#endif // NRFX_CHECK(NFCT_WORKAROUND_USES_TIMER)

                m_nfct_cb.field_on = true;
                nfct_evt.evt_id    = NRFX_NFCT_EVT_FIELD_DETECTED;
                NRFX_NFCT_CB_HANDLE(m_nfct_cb.config.cb, nfct_evt);
            }
            break;

        case NRFX_NFC_FIELD_STATE_OFF:
            if (m_nfct_cb.field_on)
            {
                nrf_nfct_task_trigger(NRF_NFCT, NRF_NFCT_TASK_SENSE);
                nrf_nfct_int_disable(NRF_NFCT, NRFX_NFCT_RX_INT_MASK | NRFX_NFCT_TX_INT_MASK);
                m_nfct_cb.field_on = false;
                nfct_evt.evt_id    = NRFX_NFCT_EVT_FIELD_LOST;

                /* Begin: Workaround for anomaly 218 */
                nrfx_nfct_frame_delay_max_set(true);
                /* End: Workaround for anomaly 218 */

                NRFX_NFCT_CB_HANDLE(m_nfct_cb.config.cb, nfct_evt);
            }
            break;

        default:
            /* No implementation required */
            break;
    }
}

#if NRFX_CHECK(NFCT_WORKAROUND_USES_TIMER)
#if NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_190)
static void nrfx_nfct_activate_check(void)
{
    static bool is_field_validation_pending = false;

    if (is_field_validation_pending)
    {
        is_field_validation_pending                  = false;
        m_timer_workaround.fieldevents_filter_active = false;

        // Check the field status and take action if field is lost.
        nrfx_nfct_field_event_handler(NRFX_NFC_FIELD_STATE_UNKNOWN);
        return;
    }

    if ((m_timer_workaround.is_hfclk_on) && (m_timer_workaround.is_delayed))
    {
        nrf_nfct_task_trigger(NRF_NFCT, NRF_NFCT_TASK_ACTIVATE);
        is_field_validation_pending = true;

        // Start the timer second time to validate whether the tag has locked to the field.
        nrfx_timer_clear(&m_timer_workaround.timer);
        nrfx_timer_enable(&m_timer_workaround.timer);
    }
}
#endif // NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_190)

#if NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_79)
/* Begin: Workaround for anomaly 116 */
static inline void nrfx_nfct_reset(void)
{
    uint32_t                       fdmax;
    uint32_t                       fdmin;
    uint32_t                       int_enabled;
    uint8_t                        nfcid1[NRF_NFCT_SENSRES_NFCID1_SIZE_TRIPLE];
    nrf_nfct_sensres_nfcid1_size_t nfcid1_size;
    nrf_nfct_selres_protocol_t     protocol;

    // Save parameter settings before the reset of the NFCT peripheral.
    fdmax       = nrf_nfct_frame_delay_max_get(NRF_NFCT);
    fdmin       = nrf_nfct_frame_delay_min_get(NRF_NFCT);
    nfcid1_size = nrf_nfct_nfcid1_get(NRF_NFCT, nfcid1);
    protocol    = nrf_nfct_selres_protocol_get(NRF_NFCT);
    int_enabled = nrf_nfct_int_enable_get(NRF_NFCT);

    // Reset the NFCT peripheral.
    *(volatile uint32_t *)0x40005FFC = 0;
    *(volatile uint32_t *)0x40005FFC;
    *(volatile uint32_t *)0x40005FFC = 1;

    // Restore parameter settings after the reset of the NFCT peripheral.
    nrf_nfct_frame_delay_max_set(NRF_NFCT, fdmax);
    nrf_nfct_frame_delay_min_set(NRF_NFCT, fdmin);
    nrf_nfct_nfcid1_set(NRF_NFCT, nfcid1, nfcid1_size);
    nrf_nfct_selres_protocol_set(NRF_NFCT, protocol);

    // Restore general HW configuration.
    nrfx_nfct_hw_init_setup();

    // Restore interrupts.
    nrf_nfct_int_enable(NRF_NFCT, int_enabled);

    // Disable interrupts associated with data exchange.
    nrf_nfct_int_disable(NRF_NFCT, NRFX_NFCT_RX_INT_MASK | NRFX_NFCT_TX_INT_MASK);

    NRFX_LOG_INFO("Reinitialize");
}
/* End: Workaround for anomaly 116 */

static void nrfx_nfct_field_poll(void)
{
    if (!nrfx_nfct_field_check())
    {
        if (++m_timer_workaround.field_state_cnt > NRFX_NFCT_FIELDLOST_THR)
        {
            nrfx_nfct_evt_t nfct_evt =
            {
                .evt_id = NRFX_NFCT_EVT_FIELD_LOST,
            };

            nrfx_timer_disable(&m_timer_workaround.timer);
            m_nfct_cb.field_on = false;

            nrfx_nfct_frame_delay_max_set(true);

            /* Begin: Workaround for anomaly 116 */
            /* resume the NFCT to initialized state */
            nrfx_nfct_reset();
            /* End: Workaround for anomaly 116 */

            NRFX_NFCT_CB_HANDLE(m_nfct_cb.config.cb, nfct_evt);
        }
        return;
    }

    m_timer_workaround.field_state_cnt = 0;
}
#endif // NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_79)

static void nrfx_nfct_field_timer_handler(nrf_timer_event_t event_type, void * p_context)
{
    (void)p_context;

    if (event_type != NRF_TIMER_EVENT_COMPARE0)
    {
        return;
    }

#if NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_190)
    m_timer_workaround.is_delayed = true;

    nrfx_timer_disable(&m_timer_workaround.timer);
    nrfx_nfct_activate_check();
#elif NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_79)
    nrfx_nfct_field_poll();
#endif // NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_190)
}

static inline nrfx_err_t nrfx_nfct_field_timer_config(void)
{
    nrfx_err_t          err_code;
    nrfx_timer_config_t timer_cfg =
    {
        .frequency          = NRF_TIMER_FREQ_1MHz,
        .mode               = NRF_TIMER_MODE_TIMER,
        .bit_width          = NRF_TIMER_BIT_WIDTH_16,
        .interrupt_priority = NRFX_NFCT_DEFAULT_CONFIG_IRQ_PRIORITY
    };

    err_code = nrfx_timer_init(&m_timer_workaround.timer,
                               &timer_cfg,
                               nrfx_nfct_field_timer_handler);
    if (err_code != NRFX_SUCCESS)
    {
        return err_code;
    }

    nrfx_timer_extended_compare(&m_timer_workaround.timer,
                                NRF_TIMER_CC_CHANNEL0,
                                nrfx_timer_us_to_ticks(&m_timer_workaround.timer,
                                                       NRFX_NFCT_TIMER_PERIOD),
                                NRF_TIMER_SHORT_COMPARE0_CLEAR_MASK,
                                true);
    return err_code;
}
#endif // NRFX_CHECK(NFCT_WORKAROUND_USES_TIMER)

static inline
nrf_nfct_sensres_nfcid1_size_t nrf_nfct_nfcid1_size_to_sensres_size(uint8_t nfcid1_size)
{
    switch (nfcid1_size)
    {
        case NRFX_NFCT_NFCID1_SINGLE_SIZE:
            return NRF_NFCT_SENSRES_NFCID1_SIZE_SINGLE;

        case NRFX_NFCT_NFCID1_DOUBLE_SIZE:
            return NRF_NFCT_SENSRES_NFCID1_SIZE_DOUBLE;

        case NRFX_NFCT_NFCID1_TRIPLE_SIZE:
            return NRF_NFCT_SENSRES_NFCID1_SIZE_TRIPLE;

        default:
            return NRF_NFCT_SENSRES_NFCID1_SIZE_DOUBLE;
    }
}

static inline void nrfx_nfct_rxtx_int_enable(uint32_t rxtx_int_mask)
{
    nrf_nfct_int_enable(NRF_NFCT, rxtx_int_mask & m_nfct_cb.config.rxtx_int_mask);
}

nrfx_err_t nrfx_nfct_init(nrfx_nfct_config_t const * p_config)
{
    NRFX_ASSERT(p_config);

    nrfx_err_t err_code = NRFX_SUCCESS;

    if (m_nfct_cb.state != NRFX_DRV_STATE_UNINITIALIZED)
    {
        return NRFX_ERROR_INVALID_STATE;
    }

    m_nfct_cb.config = *p_config;
    nrfx_nfct_hw_init_setup();

    NRFX_IRQ_PENDING_CLEAR(NFCT_IRQn);
    NRFX_IRQ_PRIORITY_SET(NFCT_IRQn, NRFX_NFCT_DEFAULT_CONFIG_IRQ_PRIORITY);
    NRFX_IRQ_ENABLE(NFCT_IRQn);

#if NRFX_CHECK(NFCT_WORKAROUND_USES_TIMER)
    /* Initialize Timer module as the workaround for NFCT HW issues. */
    err_code = nrfx_nfct_field_timer_config();
#endif // NRFX_CHECK(NFCT_WORKAROUND_USES_TIMER)

    m_nfct_cb.state           = NRFX_DRV_STATE_INITIALIZED;
    m_nfct_cb.frame_delay_max = NFCT_FRAMEDELAYMAX_DEFAULT;
    m_nfct_cb.frame_delay_min = NFCT_FRAMEDELAYMIN_DEFAULT;

    NRFX_LOG_INFO("Initialized");
    return err_code;
}

void nrfx_nfct_uninit(void)
{
    nrfx_nfct_disable();

    NRFX_IRQ_DISABLE(NFCT_IRQn);
    NRFX_IRQ_PENDING_CLEAR(NFCT_IRQn);

#if NRFX_CHECK(NFCT_WORKAROUND_USES_TIMER)
    /* De-initialize Timer module as the workaround for NFCT HW issues. */
    nrfx_timer_uninit(&m_timer_workaround.timer);
#endif // NRFX_CHECK(NFCT_WORKAROUND_USES_TIMER)

    m_nfct_cb.state = NRFX_DRV_STATE_UNINITIALIZED;
}

void nrfx_nfct_enable(void)
{
    nrf_nfct_error_status_clear(NRF_NFCT, NRFX_NFCT_ERROR_STATUS_ALL_MASK);
    nrf_nfct_task_trigger(NRF_NFCT, NRF_NFCT_TASK_SENSE);

    nrf_nfct_int_enable(NRF_NFCT, NRF_NFCT_INT_FIELDDETECTED_MASK |
                                  NRF_NFCT_INT_ERROR_MASK         |
                                  NRF_NFCT_INT_SELECTED_MASK);
#if !NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_79)
    nrf_nfct_int_enable(NRF_NFCT, NRF_NFCT_INT_FIELDLOST_MASK);
#endif // !NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_79)

    NRFX_LOG_INFO("Start");
}

void nrfx_nfct_disable(void)
{
    nrf_nfct_int_disable(NRF_NFCT, NRF_NFCT_DISABLE_ALL_INT);
    nrf_nfct_task_trigger(NRF_NFCT, NRF_NFCT_TASK_DISABLE);

    NRFX_LOG_INFO("Stop");
}

bool nrfx_nfct_field_check(void)
{
    uint32_t const field_state = nrf_nfct_field_status_get(NRF_NFCT);

    if (((field_state & NRF_NFCT_FIELD_STATE_PRESENT_MASK) == 0) &&
        ((field_state & NRF_NFCT_FIELD_STATE_LOCK_MASK) == 0))
    {
        /* Field is not active */
        return false;
    }

    return true;
}

void nrfx_nfct_rx(nrfx_nfct_data_desc_t const * p_tx_data)
{
    NRFX_ASSERT(p_tx_data);

    nrf_nfct_rxtx_buffer_set(NRF_NFCT, (uint8_t *) p_tx_data->p_data, p_tx_data->data_size);

    nrfx_nfct_rxtx_int_enable(NRFX_NFCT_RX_INT_MASK);
    nrf_nfct_task_trigger(NRF_NFCT, NRF_NFCT_TASK_ENABLERXDATA);
}

nrfx_err_t nrfx_nfct_tx(nrfx_nfct_data_desc_t const * p_tx_data,
                        nrf_nfct_frame_delay_mode_t   delay_mode)
{
    NRFX_ASSERT(p_tx_data);
    NRFX_ASSERT(p_tx_data->p_data);

    nrfx_err_t err = NRFX_SUCCESS;

    if (p_tx_data->data_size == 0)
    {
        return NRFX_ERROR_INVALID_LENGTH;
    }

    NRFX_CRITICAL_SECTION_ENTER();

    /* In case when NFC frame transmission has already started, it returns an error. */
    if (NRFX_NFCT_EVT_ACTIVE(TXFRAMESTART))
    {
        err = NRFX_ERROR_BUSY;
    }
    else
    {
        /* In case when Tx operation was scheduled with delay, stop scheduled Tx operation. */
#if defined(NRF52_SERIES)
        *(volatile uint32_t *)0x40005010 = 0x01;
#elif defined(NRF5340_XXAA_APPLICATION) && defined(NRF_TRUSTZONE_NONSECURE)
        *(volatile uint32_t *)0x4002D010 = 0x01;
#elif defined(NRF5340_XXAA_APPLICATION)
        *(volatile uint32_t *)0x5002D010 = 0x01;
#endif
        nrf_nfct_rxtx_buffer_set(NRF_NFCT, (uint8_t *) p_tx_data->p_data, p_tx_data->data_size);
        nrf_nfct_tx_bits_set(NRF_NFCT, NRFX_NFCT_BYTES_TO_BITS(p_tx_data->data_size));
        nrf_nfct_frame_delay_mode_set(NRF_NFCT, (nrf_nfct_frame_delay_mode_t) delay_mode);
        nrfx_nfct_frame_delay_max_set(false);

        nrfx_nfct_rxtx_int_enable(NRFX_NFCT_TX_INT_MASK);
        nrf_nfct_task_trigger(NRF_NFCT, NRF_NFCT_TASK_STARTTX);
    }

    NRFX_CRITICAL_SECTION_EXIT();

    if (err == NRFX_SUCCESS)
    {
        NRFX_LOG_INFO("Tx start");
    }

    return err;
}

nrfx_err_t nrfx_nfct_bits_tx(nrfx_nfct_data_desc_t const * p_tx_data,
                             nrf_nfct_frame_delay_mode_t   delay_mode)
{
    NRFX_ASSERT(p_tx_data);
    NRFX_ASSERT(p_tx_data->p_data);

    nrfx_err_t err = NRFX_SUCCESS;

    if (p_tx_data->data_size == 0)
    {
        return NRFX_ERROR_INVALID_LENGTH;
    }

    /* Get buffer length, add additional byte if bits go beyond last whole byte */
    uint32_t buffer_length = NRFX_NFCT_BITS_TO_BYTES(p_tx_data->data_size);
    if (p_tx_data->data_size & NFCT_TXD_AMOUNT_TXDATABITS_Msk)
    {
        ++buffer_length;
    }

    NRFX_CRITICAL_SECTION_ENTER();

    /* In case when NFC frame transmission has already started, it returns an error. */
    if (NRFX_NFCT_EVT_ACTIVE(TXFRAMESTART))
    {
        err = NRFX_ERROR_BUSY;
    }
    else
    {
        /* In case when Tx operation was scheduled with delay, stop scheduled Tx operation. */
#if defined(NRF52_SERIES)
        *(volatile uint32_t *)0x40005010 = 0x01;
#elif defined(NRF5340_XXAA_APPLICATION) && defined(NRF_TRUSTZONE_NONSECURE)
        *(volatile uint32_t *)0x4002D010 = 0x01;
#elif defined(NRF5340_XXAA_APPLICATION)
        *(volatile uint32_t *)0x5002D010 = 0x01;
#endif
        nrf_nfct_rxtx_buffer_set(NRF_NFCT, (uint8_t *) p_tx_data->p_data, buffer_length);
        nrf_nfct_tx_bits_set(NRF_NFCT, p_tx_data->data_size);
        nrf_nfct_frame_delay_mode_set(NRF_NFCT, (nrf_nfct_frame_delay_mode_t) delay_mode);
        nrfx_nfct_frame_delay_max_set(false);

        nrfx_nfct_rxtx_int_enable(NRFX_NFCT_TX_INT_MASK);
        nrf_nfct_task_trigger(NRF_NFCT, NRF_NFCT_TASK_STARTTX);
    }

    NRFX_CRITICAL_SECTION_EXIT();

    if (err == NRFX_SUCCESS)
    {
        NRFX_LOG_INFO("Tx start");
    }

    return err;
}

void nrfx_nfct_state_force(nrfx_nfct_state_t state)
{
#if NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_190)
    if (state == NRFX_NFCT_STATE_ACTIVATED)
    {
        m_timer_workaround.is_hfclk_on = true;
        /* NFCT will be activated based on additional conditions */
        nrfx_nfct_activate_check();
        return;
    }
#endif // NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_190)
    nrf_nfct_task_trigger(NRF_NFCT, (nrf_nfct_task_t) state);
}

void nrfx_nfct_init_substate_force(nrfx_nfct_active_state_t sub_state)
{
    if (sub_state == NRFX_NFCT_ACTIVE_STATE_DEFAULT)
    {
#if defined(NRF52832_XXAA) || defined(NRF52832_XXAB)
        if (((*(uint32_t volatile *)(0x40005420)) & 0x1UL) == (1UL))
#else
        if (nrf_nfct_sleep_state_get(NRF_NFCT) == NRF_NFCT_SLEEP_STATE_SLEEP_A)
#endif //defined(NRF52832_XXAA) || defined(NRF52832_XXAB)
        {
            // Default state is SLEEP_A
            nrf_nfct_task_trigger(NRF_NFCT, NRF_NFCT_TASK_GOSLEEP);
        }
        else
        {
            // Default state is IDLE
            nrf_nfct_task_trigger(NRF_NFCT, NRF_NFCT_TASK_GOIDLE);
        }
    }
    else
    {
        nrf_nfct_task_trigger(NRF_NFCT, (nrf_nfct_task_t) sub_state);
    }

    nrfx_nfct_frame_delay_max_set(true);

    /* Disable TX/RX here (will be enabled at SELECTED) */
    nrf_nfct_int_disable(NRF_NFCT, NRFX_NFCT_RX_INT_MASK | NRFX_NFCT_TX_INT_MASK);
}

nrfx_err_t nrfx_nfct_parameter_set(nrfx_nfct_param_t const * p_param)
{
    NRFX_ASSERT(p_param);

    switch (p_param->id)
    {
        case NRFX_NFCT_PARAM_ID_FDT:
        {
            uint32_t delay     = p_param->data.fdt;
            uint32_t delay_thr = NFCT_FRAMEDELAYMAX_FRAMEDELAYMAX_Msk;
            uint32_t delay_max;

            delay_max = (delay > delay_thr) ? delay_thr : delay;
            if (delay_max < m_nfct_cb.frame_delay_min)
            {
                return NRFX_ERROR_INVALID_PARAM;
            }

            m_nfct_cb.frame_delay_max = delay_max;
            break;
        }

        case NRFX_NFCT_PARAM_ID_FDT_MIN:
        {
            uint32_t delay = p_param->data.fdt_min;
            uint32_t delay_thr = NFCT_FRAMEDELAYMAX_FRAMEDELAYMAX_Msk;
            uint32_t delay_min;

            delay_min = (delay > delay_thr) ? delay_thr : delay;
            if (delay_min > m_nfct_cb.frame_delay_max)
            {
                return NRFX_ERROR_INVALID_PARAM;
            }

            m_nfct_cb.frame_delay_min = delay_min;
            nrf_nfct_frame_delay_min_set(NRF_NFCT, m_nfct_cb.frame_delay_min);
            break;
        }

        case NRFX_NFCT_PARAM_ID_SEL_RES:
            if (p_param->data.sel_res_protocol > NRF_NFCT_SELRES_PROTOCOL_NFCDEP_T4AT)
            {
                return NRFX_ERROR_INVALID_PARAM;
            }

            nrf_nfct_selres_protocol_set(NRF_NFCT,
                    (nrf_nfct_selres_protocol_t) p_param->data.sel_res_protocol);
            break;

        case NRFX_NFCT_PARAM_ID_NFCID1:
        {
            nrf_nfct_sensres_nfcid1_size_t id_size_mask;

            id_size_mask = nrf_nfct_nfcid1_size_to_sensres_size(p_param->data.nfcid1.id_size);
            nrf_nfct_nfcid1_set(NRF_NFCT, p_param->data.nfcid1.p_id, id_size_mask);
            break;
        }

        default:
            break;
    }

    return NRFX_SUCCESS;
}

nrfx_err_t nrfx_nfct_nfcid1_default_bytes_get(uint8_t * const p_nfcid1_buff,
                                              uint32_t        nfcid1_buff_len)
{
    if ((nfcid1_buff_len != NRFX_NFCT_NFCID1_SINGLE_SIZE) &&
        (nfcid1_buff_len != NRFX_NFCT_NFCID1_DOUBLE_SIZE) &&
        (nfcid1_buff_len != NRFX_NFCT_NFCID1_TRIPLE_SIZE))
    {
        return NRFX_ERROR_INVALID_LENGTH;
    }

#if defined(FICR_NFC_TAGHEADER0_MFGID_Msk) && !defined(NRF_TRUSTZONE_NONSECURE)
    uint32_t nfc_tag_header0 = NRF_FICR->NFC.TAGHEADER0;
    uint32_t nfc_tag_header1 = NRF_FICR->NFC.TAGHEADER1;
    uint32_t nfc_tag_header2 = NRF_FICR->NFC.TAGHEADER2;
#else
    uint32_t nfc_tag_header0 = 0x5F;
    uint32_t nfc_tag_header1 = 0;
    uint32_t nfc_tag_header2 = 0;
#endif

    p_nfcid1_buff[0] = (uint8_t) (nfc_tag_header0 >> 0);
    p_nfcid1_buff[1] = (uint8_t) (nfc_tag_header0 >> 8);
    p_nfcid1_buff[2] = (uint8_t) (nfc_tag_header0 >> 16);
    p_nfcid1_buff[3] = (uint8_t) (nfc_tag_header1 >> 0);

    if (nfcid1_buff_len != NRFX_NFCT_NFCID1_SINGLE_SIZE)
    {
        p_nfcid1_buff[4] = (uint8_t) (nfc_tag_header1 >> 8);
        p_nfcid1_buff[5] = (uint8_t) (nfc_tag_header1 >> 16);
        p_nfcid1_buff[6] = (uint8_t) (nfc_tag_header1 >> 24);

        if (nfcid1_buff_len == NRFX_NFCT_NFCID1_TRIPLE_SIZE)
        {
            p_nfcid1_buff[7] = (uint8_t) (nfc_tag_header2 >> 0);
            p_nfcid1_buff[8] = (uint8_t) (nfc_tag_header2 >> 8);
            p_nfcid1_buff[9] = (uint8_t) (nfc_tag_header2 >> 16);
        }
        /* Begin: Workaround for anomaly 181. */
        /* Workaround for wrong value in NFCID1. Value 0x88 cannot be used as byte 3
           of a double-size NFCID1, according to the NFC Forum Digital Protocol specification. */
        else if (p_nfcid1_buff[3] == 0x88)
        {
            p_nfcid1_buff[3] |= 0x11;
        }
        /* End: Workaround for anomaly 181 */
    }

    return NRFX_SUCCESS;
}

void nrfx_nfct_autocolres_enable(void)
{
#if defined(NRF52832_XXAA) || defined(NRF52832_XXAB)
    (*(uint32_t *)(0x4000559C)) &= (~(0x1UL));
#else
    nrf_nfct_autocolres_enable(NRF_NFCT);
#endif //defined(NRF52832_XXAA) || defined(NRF52832_XXAB)
}

void nrfx_nfct_autocolres_disable(void)
{
#if defined(NRF52832_XXAA) || defined(NRF52832_XXAB)
    (*(uint32_t *)(0x4000559C)) |= (0x1UL);
#else
    nrf_nfct_autocolres_disable(NRF_NFCT);
#endif //defined(NRF52832_XXAA) || defined(NRF52832_XXAB)
}

void nrfx_nfct_irq_handler(void)
{
    nrfx_nfct_field_state_t current_field = NRFX_NFC_FIELD_STATE_NONE;

    if (NRFX_NFCT_EVT_ACTIVE(FIELDDETECTED))
    {
        nrf_nfct_event_clear(NRF_NFCT, NRF_NFCT_EVENT_FIELDDETECTED);
        current_field = NRFX_NFC_FIELD_STATE_ON;

        NRFX_LOG_DEBUG("Field detected");
    }

#if !NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_79)
    if (NRFX_NFCT_EVT_ACTIVE(FIELDLOST))
    {
        nrf_nfct_event_clear(NRF_NFCT, NRF_NFCT_EVENT_FIELDLOST);
        current_field = (current_field == NRFX_NFC_FIELD_STATE_NONE) ?
                        NRFX_NFC_FIELD_STATE_OFF : NRFX_NFC_FIELD_STATE_UNKNOWN;

        NRFX_LOG_DEBUG("Field lost");
    }
#endif //!NRFX_CHECK(USE_WORKAROUND_FOR_ANOMALY_79)

    /* Perform actions if any FIELD event is active */
    if (current_field != NRFX_NFC_FIELD_STATE_NONE)
    {
        nrfx_nfct_field_event_handler(current_field);
    }

    if (NRFX_NFCT_EVT_ACTIVE(RXFRAMESTART))
    {
        nrf_nfct_event_clear(NRF_NFCT, NRF_NFCT_EVENT_RXFRAMESTART);

        nrfx_nfct_evt_t nfct_evt =
        {
            .evt_id = NRFX_NFCT_EVT_RX_FRAMESTART
        };

        NRFX_NFCT_CB_HANDLE(m_nfct_cb.config.cb, nfct_evt);
    }

    if (NRFX_NFCT_EVT_ACTIVE(RXFRAMEEND))
    {
        nrf_nfct_event_clear(NRF_NFCT, NRF_NFCT_EVENT_RXFRAMEEND);

        nrfx_nfct_evt_t nfct_evt =
        {
            .evt_id = NRFX_NFCT_EVT_RX_FRAMEEND
        };

        /* Take into account only the number of whole bytes. */
        nfct_evt.params.rx_frameend.rx_status         = 0;
        nfct_evt.params.rx_frameend.rx_data.p_data    = nrf_nfct_rxtx_buffer_get(NRF_NFCT);
        nfct_evt.params.rx_frameend.rx_data.data_size =
            NRFX_NFCT_BITS_TO_BYTES(nrf_nfct_rx_bits_get(NRF_NFCT, true));

        if (NRFX_NFCT_EVT_ACTIVE(RXERROR))
        {
            nfct_evt.params.rx_frameend.rx_status =
                (nrf_nfct_rx_frame_status_get(NRF_NFCT) & NRFX_NFCT_FRAME_STATUS_RX_ALL_MASK);
            nrf_nfct_event_clear(NRF_NFCT, NRF_NFCT_EVENT_RXERROR);

            NRFX_LOG_DEBUG("Rx error (0x%x)", (unsigned int) nfct_evt.params.rx_frameend.rx_status);

            /* Clear rx frame status */
            nrf_nfct_rx_frame_status_clear(NRF_NFCT, NRFX_NFCT_FRAME_STATUS_RX_ALL_MASK);
        }

        NRFX_NFCT_CB_HANDLE(m_nfct_cb.config.cb, nfct_evt);

        NRFX_LOG_DEBUG("Rx fend");
    }

    if (NRFX_NFCT_EVT_ACTIVE(SELECTED))
    {
        nrf_nfct_event_clear(NRF_NFCT, NRF_NFCT_EVENT_SELECTED);
        /* Clear also RX END and RXERROR events because SW does not take care of
           commands that were received before selecting the tag. */
        nrf_nfct_event_clear(NRF_NFCT, NRF_NFCT_EVENT_RXFRAMEEND);
        nrf_nfct_event_clear(NRF_NFCT, NRF_NFCT_EVENT_RXERROR);
        nrf_nfct_event_clear(NRF_NFCT, NRF_NFCT_EVENT_TXFRAMESTART);
        nrf_nfct_event_clear(NRF_NFCT, NRF_NFCT_EVENT_TXFRAMEEND);

        nrfx_nfct_frame_delay_max_set(false);

        /* At this point any previous error status can be ignored. */
        nrf_nfct_rx_frame_status_clear(NRF_NFCT, NRFX_NFCT_FRAME_STATUS_RX_ALL_MASK);
        nrf_nfct_error_status_clear(NRF_NFCT, NRFX_NFCT_ERROR_STATUS_ALL_MASK);

        nrfx_nfct_evt_t nfct_evt =
        {
            .evt_id = NRFX_NFCT_EVT_SELECTED
        };
        NRFX_NFCT_CB_HANDLE(m_nfct_cb.config.cb, nfct_evt);

        NRFX_LOG_DEBUG("Selected");
    }

    if (NRFX_NFCT_EVT_ACTIVE(ERROR))
    {
        uint32_t err_status = nrf_nfct_error_status_get(NRF_NFCT);
        nrf_nfct_event_clear(NRF_NFCT, NRF_NFCT_EVENT_ERROR);

        nrfx_nfct_evt_t nfct_evt =
        {
            .evt_id = NRFX_NFCT_EVT_ERROR
        };

        /* Clear FRAMEDELAYTIMEOUT error (expected HW behaviour) when SLP_REQ command was received. */
        if (err_status & NRF_NFCT_ERROR_FRAMEDELAYTIMEOUT_MASK)
        {
            nrf_nfct_error_status_clear(NRF_NFCT, NRF_NFCT_ERROR_FRAMEDELAYTIMEOUT_MASK);

            nfct_evt.params.error.reason = NRFX_NFCT_ERROR_FRAMEDELAYTIMEOUT;
            NRFX_NFCT_CB_HANDLE(m_nfct_cb.config.cb, nfct_evt);
        }

        /* Report any other error. */
        err_status &= ~NRF_NFCT_ERROR_FRAMEDELAYTIMEOUT_MASK;
        if (err_status)
        {
            NRFX_LOG_DEBUG("Error (0x%x)", (unsigned int) err_status);
        }

        /* Clear error status. */
        nrf_nfct_error_status_clear(NRF_NFCT, NRFX_NFCT_ERROR_STATUS_ALL_MASK);
    }

    if (NRFX_NFCT_EVT_ACTIVE(TXFRAMESTART))
    {
        nrf_nfct_event_clear(NRF_NFCT, NRF_NFCT_EVENT_TXFRAMESTART);

        if (m_nfct_cb.config.cb != NULL)
        {
            nrfx_nfct_evt_t nfct_evt;

            nfct_evt.evt_id                                 = NRFX_NFCT_EVT_TX_FRAMESTART;
            nfct_evt.params.tx_framestart.tx_data.p_data    = nrf_nfct_rxtx_buffer_get(NRF_NFCT);
            nfct_evt.params.tx_framestart.tx_data.data_size =
                NRFX_NFCT_BITS_TO_BYTES(nrf_nfct_tx_bits_get(NRF_NFCT));

            m_nfct_cb.config.cb(&nfct_evt);
        }
    }

    if (NRFX_NFCT_EVT_ACTIVE(TXFRAMEEND))
    {
        nrf_nfct_event_clear(NRF_NFCT, NRF_NFCT_EVENT_TXFRAMEEND);

        nrfx_nfct_evt_t nfct_evt =
        {
            .evt_id = NRFX_NFCT_EVT_TX_FRAMEEND
        };

        /* Ignore any frame transmission until a new TX is scheduled by nrfx_nfct_tx() */
        nrf_nfct_int_disable(NRF_NFCT, NRFX_NFCT_TX_INT_MASK);

        NRFX_NFCT_CB_HANDLE(m_nfct_cb.config.cb, nfct_evt);

        NRFX_LOG_DEBUG("Tx fend");
    }
}

#endif // NRFX_CHECK(NRFX_NFCT_ENABLED)
