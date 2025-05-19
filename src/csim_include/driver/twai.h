#pragma once
#ifdef CSIM
#include "Arduino.h"
typedef struct {
    union {
        struct {
            //The order of these bits must match deprecated message flags for compatibility reasons
            uint32_t extd: 1;           /**< Extended Frame Format (29bit ID) */
            uint32_t rtr: 1;            /**< Message is a Remote Frame */
            uint32_t ss: 1;             /**< Transmit as a Single Shot Transmission. Unused for received. */
            uint32_t self: 1;           /**< Transmit as a Self Reception Request. Unused for received. */
            uint32_t dlc_non_comp: 1;   /**< Message's Data length code is larger than 8. This will break compliance with ISO 11898-1 */
            uint32_t reserved: 27;      /**< Reserved bits */
        };
        //Todo: Deprecate flags
        uint32_t flags;                 /**< Deprecated: Alternate way to set bits using message flags */
    };
    uint32_t identifier;                /**< 11 or 29 bit identifier */
    uint8_t data_length_code;           /**< Data length code */
    uint8_t data[8];    /**< Data bytes (not relevant in RTR frame) */
} twai_message_t;


typedef int twai_mode_t;

typedef struct {
    int dummy; 
} twai_filter_config_t;

typedef struct {
    int controller_id;              /**< TWAI controller ID, index from 0.
                                         If you want to install TWAI driver with a non-zero controller_id, please use `twai_driver_instal
l_v2` */
    twai_mode_t mode;               /**< Mode of TWAI controller */
    gpio_num_t tx_io;               /**< Transmit GPIO number */
    gpio_num_t rx_io;               /**< Receive GPIO number */
    gpio_num_t clkout_io;           /**< CLKOUT GPIO number (optional, set to -1 if unused) */
    gpio_num_t bus_off_io;          /**< Bus off indicator GPIO number (optional, set to -1 if unused) */
    uint32_t tx_queue_len;          /**< Number of messages TX queue can hold (set to 0 to disable TX Queue) */
    uint32_t rx_queue_len;          /**< Number of messages RX queue can hold */
    uint32_t alerts_enabled;        /**< Bit field of alerts to enable (see documentation) */
    uint32_t clkout_divider;        /**< CLKOUT divider. Can be 1 or any even number from 2 to 14 (optional, set to 0 if unused) */
    int intr_flags;                 /**< Interrupt flags to set the priority of the driver's ISR. Note that to use the ESP_INTR_FLAG_IRAM
, the CONFIG_TWAI_ISR_IN_IRAM option should be enabled first. */
    struct {
        uint32_t sleep_allow_pd;    /**< Set to allow power down. When this flag set, the driver will backup/restore the TWAI registers b
efore/after entering/exist sleep mode.
                                         By this approach, the system can power off TWAI's power domain.
                                         This can save power, but at the expense of more RAM being consumed. */
    } general_flags;                /**< General flags */
    // Ensure TWAI_GENERAL_CONFIG_DEFAULT_V2 is updated and in order if new fields are added
} twai_general_config_t;

typedef struct { 
    int dummy;
} twai_timing_config_t;
typedef struct { 
    int msgs_to_tx;
    int msgs_to_rx;
} twai_status_info_t;

int twai_driver_install(twai_general_config_t *, twai_timing_config_t *, twai_filter_config_t *);
int twai_start();
int twai_driver_uninstall();
int twai_stop();

int twai_receive(twai_message_t *, int);
int twai_transmit(twai_message_t *, int);

int twai_get_status_info(twai_status_info_t *);

#define TWAI_MODE_NORMAL 0
#define TWAI_MODE_LISTEN_ONLY 0
#define TWAI_IO_UNUSED 0 
#define TWAI_ALERT_NONE 0 
#define ESP_INTR_FLAG_LEVEL1 0 

#define TWAI_TIMING_CONFIG_100KBITS() {0}
#define TWAI_TIMING_CONFIG_125KBITS() {0}
#define TWAI_TIMING_CONFIG_250KBITS() {0}
#define TWAI_TIMING_CONFIG_500KBITS() {0}
#define TWAI_TIMING_CONFIG_800KBITS() {0}
#define TWAI_TIMING_CONFIG_1MBITS() {0}
#define TWAI_FILTER_CONFIG_ACCEPT_ALL() {0}
#endif
