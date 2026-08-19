#include "CONFIG.h"
#include "broadcaster.h"

// Advertisement interval in 625 us units (30 seconds by default).
#define DEFAULT_ADVERTISING_INTERVAL (160U * 30U)

// Rotate to the next key once per hour. TMOS timers use 625 us units.
#define KEY_ROTATION_PERIOD (60UL * 60UL * 1600UL)
#define NETWORK_DWELL_PERIOD (30UL * 1600UL)
#define ADV_RESTART_DELAY   600U

#define FINDMY_KEY_LENGTH 28U
#define FINDMY_MAX_KEYS   32U
#define GOOGLE_EID_LENGTH 20U

#define NETWORK_APPLE  1U
#define NETWORK_GOOGLE 2U
#define NETWORK_DUAL   3U

/*
 * prep_fw.py replaces this marker and the zero-filled slots following it with
 * up to FINDMY_MAX_KEYS consecutive 28-byte advertisement public keys.
 */
static const uint8_t public_keys[FINDMY_MAX_KEYS * FINDMY_KEY_LENGTH] =
    "OFFLINEFINDINGPUBLICKEYHERE!";

/* prep_fw.py replaces this marker with Google's 20-byte advertisement EID. */
static const uint8_t google_eid[GOOGLE_EID_LENGTH] =
    "GOOGLEFINDMYEIDHERE!";

/*
 * Keep patchable settings in data rather than relying on compiler-specific
 * instruction patterns. The marker is exactly 16 bytes and is followed by a
 * little-endian uint16_t advertising interval and a uint8_t network mode.
 */
static volatile const struct
{
    uint8_t marker[16];
    uint16_t advertising_interval;
    uint8_t network_mode;
} firmware_config = {
    "FINDMY_CONFIG_V1",
    DEFAULT_ADVERTISING_INTERVAL,
    NETWORK_APPLE,
};

static uint8_t Broadcaster_TaskID;
static uint8_t key_count;
static uint8_t current_key;
static uint8_t bt_addr[6];
static uint8_t configured_networks;
static uint8_t active_network;
static uint8_t address_change_pending;

static uint8_t appleAdvertData[] = {
    0x1e,       /* Length (30) */
    0xff,       /* Manufacturer Specific Data */
    0x4c, 0x00, /* Apple company ID */
    0x12, 0x19, /* Offline Finding type and length */
    0x00,       /* State */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, /* First two public-key bits */
    0x00, /* Hint */
};

/*
 * Google Find My Device / Find Hub Network frame from GoogleFindMyTools.
 * It is a Flags AD structure followed by FEAA 16-bit service data:
 * frame type 0x41, a 20-byte EID, and hashed flags.
 */
static uint8_t googleAdvertData[] = {
    0x02, 0x01, 0x06, /* General-discoverable, BR/EDR-not-supported flags */
    0x19, 0x16,       /* 25-byte Service Data - 16-bit UUID structure */
    0xaa, 0xfe,       /* FEAA, little endian */
    0x41,             /* FMDN frame with unwanted-tracking protection */
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, /* 20-byte EID */
    0x00,                         /* Hashed flags */
};

static void Broadcaster_ProcessTMOSMsg(tmos_event_hdr_t *pMsg);
static void Broadcaster_StateNotificationCB(gapRole_States_t newState);

static gapRolesBroadcasterCBs_t Broadcaster_BroadcasterCBs = {
    Broadcaster_StateNotificationCB,
    NULL,
};

static const uint8_t *key_at(uint8_t index)
{
    return &public_keys[(uint16_t)index * FINDMY_KEY_LENGTH];
}

static uint8_t key_is_empty(const uint8_t *key)
{
    uint8_t combined = 0;
    uint8_t i;

    for(i = 0; i < FINDMY_KEY_LENGTH; i++)
    {
        combined |= key[i];
    }

    return combined == 0;
}

static uint8_t find_key_count(void)
{
    uint8_t count;

    for(count = 0; count < FINDMY_MAX_KEYS; count++)
    {
        if(key_is_empty(key_at(count)))
        {
            break;
        }
    }

    // The compiled-in marker keeps an unpatched image deterministic.
    return count == 0 ? 1 : count;
}

static void apply_apple_key(uint8_t index)
{
    const uint8_t *key = key_at(index);
    uint8_t i;

    memcpy(&appleAdvertData[7], &key[6], 22);
    appleAdvertData[29] = key[0] >> 6;

    // GAP_ConfigDeviceAddr expects the address in little-endian order.
    for(i = 0; i < 6; i++)
    {
        bt_addr[5 - i] = key[i];
    }
    bt_addr[5] |= 0xc0;
}

static uint8_t current_advertising_length(void)
{
    return active_network == NETWORK_GOOGLE ? sizeof(googleAdvertData)
                                             : sizeof(appleAdvertData);
}

static uint8_t *current_advertising_data(void)
{
    return active_network == NETWORK_GOOGLE ? googleAdvertData
                                             : appleAdvertData;
}

static void update_advertising_data(void)
{
    GAP_UpdateAdvertisingData(0, TRUE, current_advertising_length(),
                              current_advertising_data());
}

static void stop_and_schedule_restart(uint8_t change_address)
{
    uint8_t advertising_enable = FALSE;

    address_change_pending = change_address;
    GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(advertising_enable),
                         &advertising_enable);
    update_advertising_data();

    /* Let the controller stop advertising before changing its address. */
    tmos_start_task(Broadcaster_TaskID, SBP_ADV_RESTART_EVT,
                    ADV_RESTART_DELAY);
}

void Broadcaster_Prepare(uint8_t mac_addr[6])
{
    uint8_t i;

    key_count = find_key_count();
    current_key = 0;
    configured_networks = firmware_config.network_mode;
    if(configured_networks < NETWORK_APPLE || configured_networks > NETWORK_DUAL)
    {
        configured_networks = NETWORK_APPLE;
    }

    active_network = configured_networks == NETWORK_GOOGLE ? NETWORK_GOOGLE
                                                            : NETWORK_APPLE;
    memcpy(&googleAdvertData[8], google_eid, GOOGLE_EID_LENGTH);

    if(configured_networks == NETWORK_GOOGLE)
    {
        uint8_t factory_addr[6];

        /*
         * The Google reference firmwares use their public/identity address;
         * the advertisement key belongs only in the FMDN service data.
         * GetMACAddress returns the little-endian controller representation.
         */
        GetMACAddress(factory_addr);
        for(i = 0; i < 6; i++)
        {
            mac_addr[i] = factory_addr[5 - i];
        }
        return;
    }

    apply_apple_key(current_key);

    // CH59x_BLEInit reverses this array when copying it into the BLE config.
    for(i = 0; i < 6; i++)
    {
        mac_addr[i] = bt_addr[5 - i];
    }
}

void Broadcaster_Init(void)
{
    uint8_t initial_advertising_enable = TRUE;
    uint8_t initial_adv_event_type = GAP_ADTYPE_ADV_NONCONN_IND;
    uint16_t adv_interval = firmware_config.advertising_interval;

    Broadcaster_TaskID = TMOS_ProcessEventRegister(Broadcaster_ProcessEvent);

    GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(initial_advertising_enable),
                         &initial_advertising_enable);
    GAPRole_SetParameter(GAPROLE_ADV_EVENT_TYPE, sizeof(initial_adv_event_type),
                         &initial_adv_event_type);
    GAPRole_SetParameter(GAPROLE_ADVERT_DATA, current_advertising_length(),
                         current_advertising_data());

    GAP_SetParamValue(TGAP_DISC_ADV_INT_MIN, adv_interval);
    GAP_SetParamValue(TGAP_DISC_ADV_INT_MAX, adv_interval);

    tmos_set_event(Broadcaster_TaskID, SBP_START_DEVICE_EVT);
}

uint16_t Broadcaster_ProcessEvent(uint8_t task_id, uint16_t events)
{
    (void)task_id;

    if(events & SYS_EVENT_MSG)
    {
        uint8_t *pMsg;

        if((pMsg = tmos_msg_receive(Broadcaster_TaskID)) != NULL)
        {
            Broadcaster_ProcessTMOSMsg((tmos_event_hdr_t *)pMsg);
            tmos_msg_deallocate(pMsg);
        }

        return events ^ SYS_EVENT_MSG;
    }

    if(events & SBP_START_DEVICE_EVT)
    {
        GAPRole_BroadcasterStartDevice(&Broadcaster_BroadcasterCBs);
        if(key_count > 1)
        {
            tmos_start_reload_task(Broadcaster_TaskID, SBP_PERIODIC_EVT,
                                   KEY_ROTATION_PERIOD);
        }
        if(configured_networks == NETWORK_DUAL)
        {
            tmos_start_reload_task(Broadcaster_TaskID,
                                   SBP_NETWORK_SWITCH_EVT,
                                   NETWORK_DWELL_PERIOD);
        }

        return events ^ SBP_START_DEVICE_EVT;
    }

    if(events & (SBP_PERIODIC_EVT | SBP_NETWORK_SWITCH_EVT))
    {
        uint16_t handled_events = events &
            (SBP_PERIODIC_EVT | SBP_NETWORK_SWITCH_EVT);
        uint8_t change_address = FALSE;

        /* Both periodic timers coincide every hour; apply both atomically. */
        if(handled_events & SBP_PERIODIC_EVT)
        {
            current_key = (current_key + 1) % key_count;
            apply_apple_key(current_key);
            change_address = TRUE;
        }
        if(handled_events & SBP_NETWORK_SWITCH_EVT)
        {
            active_network = active_network == NETWORK_APPLE ? NETWORK_GOOGLE
                                                             : NETWORK_APPLE;
        }
        /* Switching payloads must not also reinterpret or rotate the address. */
        stop_and_schedule_restart(change_address);
        return events ^ handled_events;
    }

    if(events & SBP_ADV_RESTART_EVT)
    {
        uint8_t advertising_enable = TRUE;

        if(address_change_pending)
        {
            /* Apple key addresses have the static-random 0b11 prefix. */
            GAP_ConfigDeviceAddr(ADDRTYPE_STATIC, bt_addr);
            address_change_pending = FALSE;
        }
        GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(advertising_enable),
                             &advertising_enable);
        return events ^ SBP_ADV_RESTART_EVT;
    }

    return 0;
}

static void Broadcaster_ProcessTMOSMsg(tmos_event_hdr_t *pMsg)
{
    (void)pMsg;
}

static void Broadcaster_StateNotificationCB(gapRole_States_t newState)
{
    switch(newState)
    {
        case GAPROLE_STARTED:
            PRINT("Initialized..\n");
            break;

        case GAPROLE_ADVERTISING:
            PRINT("Advertising..\n");
            break;

        case GAPROLE_WAITING:
            PRINT("Waiting for advertising..\n");
            break;

        case GAPROLE_ERROR:
            PRINT("Error..\n");
            break;

        default:
            break;
    }
}
