#include "RadarReceiver.h"

#if defined(USE_RADAR)

#include "user_interface.h"
#include "libopendroneid/opendroneid.h"
#include "libopendroneid/odid_wifi.h"
#include "logging.h"

#if defined(PLATFORM_ESP8266)

typedef struct {
	signed rssi: 8;
	unsigned rate: 4;
	unsigned is_group: 1;
	unsigned: 1;
	unsigned sig_mode: 2;
	unsigned legacy_length: 12;
	unsigned damatch0: 1;
	unsigned damatch1: 1;
	unsigned bssidmatch0: 1;
	unsigned bssidmatch1: 1;
	unsigned MCS: 7;
	unsigned CWB: 1;
	unsigned HT_length: 16;
	unsigned Smoothing: 1;
	unsigned Not_Sounding: 1;
	unsigned: 1;
	unsigned Aggregation: 1;
	unsigned STBC: 2;
	unsigned FEC_CODING: 1;
	unsigned SGI: 1;
	unsigned rxend_state: 8;
	unsigned ampdu_cnt: 8;
	unsigned channel: 4;
	unsigned: 12;
} wifi_pkt_rx_ctrl_t;

typedef struct {
	wifi_pkt_rx_ctrl_t rx_ctrl;
	uint8_t payload[0]; /* ieee80211 packet buff */
} wifi_promiscuous_pkt_t;

#endif // PLATFORM_ESP8266

#define IEEE80211_ELEMID_SSID		0x00
#define IEEE80211_ELEMID_RATES		0x01
#define IEEE80211_ELEMID_VENDOR		0xDD

#define RADAR_PILOT_TRACK_CNT   3
#define RADAR_PILOT_TIMEOUT_MS  30000

RadarPilotInfo pilots[RADAR_PILOT_TRACK_CNT];
static uint8_t radarLastDirty;

static RadarPilotInfo *Radar_FindOrAddPilotByMac(uint8_t *mac)
{
    uint32_t oldest = pilots[0].last_seen;
    uint8_t oldestIdx = 0;
    RadarPilotInfo *p;
    for (uint8_t idx=0; idx<RADAR_PILOT_TRACK_CNT; ++idx)
    {
        p = &pilots[idx];
        // Store the oldest item in case add is needed
        if (p->last_seen < oldest)
        {
            oldest = p->last_seen;
            oldestIdx = idx;
        }
        if (memcmp(mac, p->mac, MAC_LEN) == 0)
            return p;
    }

    // Not found, replace the oldest
    p = &pilots[oldestIdx];
    memset(p, 0, sizeof(*p));
    memcpy(p->mac, mac, MAC_LEN);
    return p;
}

uint8_t Radar_FindNextDirtyIdx()
{
    uint8_t retVal = radarLastDirty;
    do
    {
        retVal = (retVal + 1) % RADAR_PILOT_TRACK_CNT;
        if (pilots[retVal].dirty)
        {
            radarLastDirty = retVal;
            return retVal;
        }
    } while (retVal != radarLastDirty);

    return RADAR_PILOT_IDX_INVALID;
}

static void wifi_promiscuous_cb(uint8 *buf, uint16 len)
{
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    ieee80211_mgmt *hdr = (ieee80211_mgmt *)pkt->payload;

    // Looking for a beacon frame, 0x8000
    // MAC of transmitter is in hdr.sa
    if (hdr->frame_control != htons(0x8000))
        return;

    RadarPilotInfo *pilot = Radar_FindOrAddPilotByMac(hdr->sa);

    size_t payloadlen = len - sizeof(wifi_pkt_rx_ctrl_t);
    size_t offset = sizeof(ieee80211_mgmt) + sizeof(ieee80211_beacon);
    bool found_odid = false;
    char *ssid;
    size_t ssid_len;
    while (offset < payloadlen && !found_odid)
    {
        switch (pkt->payload[offset]) // ieee80211 element_id
        {
        case IEEE80211_ELEMID_SSID:
            {
                // Operator ID is in the SSID field
                ieee80211_ssid *iessid = (ieee80211_ssid *)&pkt->payload[offset];
                ssid_len = std::min((uint8_t)RADAR_ID_LEN, iessid->length);
                ssid = (char *)iessid->ssid;
                offset += sizeof(ieee80211_ssid) + iessid->length;
                break;
            }
        case IEEE80211_ELEMID_RATES:
            {
                ieee80211_supported_rates *iesr = (ieee80211_supported_rates *)&pkt->payload[offset];
                // ODID beacon reports 1 rate of 6mbit
                if (iesr->length > 1 || iesr->supported_rates != 0x8C)
                    return;
                offset += sizeof(ieee80211_supported_rates);
                break;
            }
        case IEEE80211_ELEMID_VENDOR:
            {
                ieee80211_vendor_specific *ievs = (ieee80211_vendor_specific *)&pkt->payload[offset];
                // The OUI of opendroneid is FA-0B-BC
                if (ievs->oui[0] != 0xfa || ievs->oui[1] != 0x0b || ievs->oui[2] != 0xbc || ievs->oui_type != 0x0d)
                    return;
                offset += sizeof(ieee80211_vendor_specific);
                found_odid = true;
                break;
            }
        default:
            // Any unexpected element_id means we can pound sand
            return;
        } // switch
    } // while (<len)

    if (!found_odid)
        return;

    //DBGLN("Found ODID: %s", pilot.operator_id);

    memcpy(pilot->operator_id, ssid, ssid_len);
    pilot->operator_id[ssid_len] = '\0';
    pilot->rssi = pkt->rx_ctrl.rssi;
    pilot->last_seen = millis();

    //ODID_service_info *si = (struct ODID_service_info *)&pkt->payload[offset];
    // can use si->counter to maybe do link quality, by detecting missing packet
    offset += sizeof(ODID_service_info);

    // Espressif are assholes for not giving us the whole packet, can't decode it because it is short
    ODID_MessagePack_encoded *msg_pack_enc = (ODID_MessagePack_encoded *)&pkt->payload[offset];
    offset += 3; // Version/MessageType SingleMessageSize MsgPackSize
    //size_t expected_msg_size = sizeof(*msg_pack_enc) - ODID_MESSAGE_SIZE * (ODID_PACK_MAX_MESSAGES - msg_pack_enc->MsgPackSize);
    //DBGLN("Found ODID: %s (%d dBm)", pilot.operator_id, pilot.rssi);
    for (unsigned i=0; i<msg_pack_enc->MsgPackSize && (offset+ODID_MESSAGE_SIZE)<payloadlen; ++i)
    {
        uint8_t MessageType = decodeMessageType(msg_pack_enc->Messages[i].rawData[0]);
        if (MessageType == ODID_MESSAGETYPE_LOCATION)
        {
            ODID_Location_encoded *loc = (ODID_Location_encoded *)&msg_pack_enc->Messages[i];
            // Conveniently, lat/lon are already 10M scale integer which is our format
            pilot->latitude = loc->Latitude;
            pilot->longitude = loc->Longitude;
            // Altitude is encoded 2x scale + 1000
            pilot->altitude = loc->Height / 2 - 1000;
            // TODO: Fix these units
            pilot->heading = loc->Direction;
            pilot->speed = loc->SpeedHorizontal;
            break;
        }
        offset += msg_pack_enc->SingleMessageSize;
        // off=94 len=116, means there's 22 of 25 bytes in msg[1]
        //DBGLN("off=%u len=%u", offset, payloadlen);
    }

    pilot->dirty = 1;
}

void RadarRx_Begin()
{
    wifi_promiscuous_enable(0);
    wifi_set_promiscuous_rx_cb(&wifi_promiscuous_cb);
    wifi_promiscuous_enable(1);
}

void RadarRx_End()
{
    wifi_promiscuous_enable(0);
    //wifi_set_promiscuous_rx_cb(nullptr);
}

#endif