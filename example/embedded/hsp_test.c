/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */
 
// *****************************************************************************
//
// Minimal setup for HSP Audio Gateway
//
// *****************************************************************************

#include "btstack-config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <btstack/hci_cmds.h>
#include <btstack/run_loop.h>

#include "hci.h"
#include "btstack_memory.h"
#include "hci_dump.h"
#include "l2cap.h"
#include "sdp_query_rfcomm.h"
#include "sdp.h"
#include "debug.h"

#define RFCOMM_SERVER_CHANNEL 1

#define HSP_HS_BUTTON_PRESS "AT+CKPD=200\r"
#define HSP_AG_OK "\r\nOK\r\n"
#define HSP_AG_ERROR "\r\nERROR\r\n"
#define HSP_AG_RING "\r\nRING\r\n"
#define HSP_MICROPHONE_GAIN "+VGM"
#define HSP_SPEAKER_GAIN "+VGS"

#define HSP_HS_MICROPHONE_GAIN "AT+VGM="
#define HSP_HS_SPEAKER_GAIN "AT+VGS="

static const char default_hsp_ag_service_name[] = "Audio Gateway";
static const char default_hsp_hs_service_name[] = "Headset";

static bd_addr_t remote = {0x00, 0x21, 0x3C, 0xAC, 0xF7, 0x38};
static uint8_t channel_nr = 0;

static uint16_t mtu;
static uint16_t rfcomm_cid = 0;
static uint16_t sco_handle = 0;
static uint16_t rfcomm_handle = 0;

// static uint8_t connection_state = 0;

static int ag_microphone_gain = -1;
static int ag_speaker_gain = -1;
static uint8_t ag_ring = 0;
static uint8_t ag_send_ok = 0;
static uint8_t ag_send_error = 0;

static uint8_t   hsp_service_buffer[150];

typedef enum {
    HSP_AudioGateway,
    HSP_Headset
} hsp_role_t;

typedef enum {
    HSP_IDLE,
    HSP_QUERY_SDP_CHANNEL,
    HSP_W2_CONNECT_SCO,
    HSP_W4_SCO_CONNECTED,
    HSP_AG_ACTIVE,
    HSP_AG_SEND_DISCONNECT
} hsp_state_t;

static hsp_state_t hsp_state = HSP_IDLE;
static hsp_role_t  hsp_role = HSP_Headset;

static void hsp_run();

// remote audio volume control
// AG +VGM=13 [0..15] ; HS AT+VGM=6 | AG OK

void hsp_ag_init(){
    hsp_state = HSP_IDLE;
    
    rfcomm_cid = 0;
    rfcomm_handle = 0;
    sco_handle = 0;

    ag_send_ok = 0;
    ag_send_error = 0;
    ag_ring = 0;
    ag_microphone_gain = -1;
    ag_speaker_gain = -1;
}

void hsp_hs_init(){
    hsp_state = HSP_IDLE;
    
    rfcomm_cid = 0;
    rfcomm_handle = 0;
    sco_handle = 0;

    // TODO
}

void hsp_hs_send_button_press(){}

void hsp_ag_connect(bd_addr_t bd_addr){
    if (hsp_state != HSP_IDLE) return;
    hsp_state = HSP_QUERY_SDP_CHANNEL;
    memcpy(remote, bd_addr, 6);
    hsp_run();
}

void hsp_hs_connect(bd_addr_t bd_addr){
    if (hsp_state != HSP_IDLE) return;
    hsp_state = HSP_QUERY_SDP_CHANNEL;
    memcpy(remote, bd_addr, 6);
    hsp_run();
}

void hsp_ag_disconnect(bd_addr_t bd_addr){
    if (hsp_state == HSP_IDLE) return;
    hsp_state = HSP_AG_SEND_DISCONNECT;
    memcpy(remote, bd_addr, 6);
    hsp_run();
}

void hsp_ag_ring(){
    if (hsp_state != HSP_AG_ACTIVE) return;

}

void hsp_ag_set_microphone_gain(uint8_t gain){
    if (gain < 0 || gain >15) {
        printf("Gain must be in interval [0..15], it is given %d\n", gain);
        return; 
    }
    ag_microphone_gain = gain;
    hsp_run();
}; 

// AG +VGS=5  [0..15] ; HS AT+VGM=6 | AG OK
void hsp_ag_set_speaker_gain(uint8_t gain){
    if (gain < 0 || gain >15) {
        printf("Gain must be in interval [0..15], it is given %d\n", gain);
        return; 
    }
    ag_speaker_gain = gain;
    hsp_run();
};    

static int send_str_over_rfcomm(uint16_t cid, char * command){
    if (!rfcomm_can_send_packet_now(rfcomm_cid)) return 1;
    int err = rfcomm_send_internal(cid, (uint8_t*) command, strlen(command));
    if (err){
        printf("rfcomm_send_internal -> error 0X%02x", err);
    }
    return err;
}


static void hsp_run(){
    int err;

    switch (hsp_state){
        case HSP_QUERY_SDP_CHANNEL:
            hsp_state = HSP_W2_CONNECT_SCO;
            switch (hsp_role){
                case HSP_AudioGateway:
                    sdp_query_rfcomm_channel_and_name_for_uuid(remote, SDP_Headset_HS);
                    break;
                case HSP_Headset:
                    sdp_query_rfcomm_channel_and_name_for_uuid(remote, SDP_Headset_AG);
                    break;
            }
            break;
        case HSP_W2_CONNECT_SCO:
            if (!hci_can_send_command_packet_now()) break;
            hsp_state = HSP_W4_SCO_CONNECTED;
            hci_send_cmd(&hci_setup_synchronous_connection_command, rfcomm_handle, 8000, 8000, 0xFFFF, 0x0060, 0xFF, 0x003F);
            break;
        case HSP_AG_SEND_DISCONNECT:
            hsp_state = HSP_IDLE;
            rfcomm_disconnect_internal(rfcomm_cid);
            rfcomm_cid = 0;
            break;
        case HSP_AG_ACTIVE:
            if (ag_send_ok){
                err = send_str_over_rfcomm(rfcomm_cid, HSP_AG_OK);
                if (!err) ag_send_ok = 0;
                break;
            }
            if (ag_send_error){
                err = send_str_over_rfcomm(rfcomm_cid, HSP_AG_ERROR);
                if (!err) ag_send_error = 0;
                break;
            }
            if (ag_ring){
                err = send_str_over_rfcomm(rfcomm_cid, HSP_AG_RING);
                if (!err) ag_ring = 0;
                break;
            }
            
            if (ag_microphone_gain >= 0){
                char buffer[10];
                sprintf(buffer, "%s=%d\r", HSP_MICROPHONE_GAIN, ag_microphone_gain);
                err = send_str_over_rfcomm(rfcomm_cid, buffer);
                if (!err) ag_microphone_gain = -1;
                break;
            }

            if (ag_speaker_gain >= 0){
                char buffer[10];
                sprintf(buffer, "%s=%d\r", HSP_SPEAKER_GAIN, ag_speaker_gain);
                err = send_str_over_rfcomm(rfcomm_cid, buffer);
                if (!err) ag_speaker_gain = -1;
                break;
            }
            break;
        default:
            break;
    }
}


static void packet_handler (void * connection, uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    // printf("packet_handler type %u, packet[0] %x\n", packet_type, packet[0]);
    if (packet_type == RFCOMM_DATA_PACKET){
        if (strncmp((char *)packet, HSP_HS_BUTTON_PRESS, strlen(HSP_HS_BUTTON_PRESS)) == 0){
            printf("Received button press %s\n", HSP_HS_BUTTON_PRESS);
            ag_send_ok = 1;
        } else if (strncmp((char *)packet, HSP_HS_MICROPHONE_GAIN, 7) == 0 ||
            strncmp((char *)packet, HSP_HS_SPEAKER_GAIN, 7) == 0){
            // uint8_t gain = packet[8];
            // TODO: parse gain
            printf("Received changed gain info %c\n", packet[8]);
            ag_send_ok = 1;
        } else if (strncmp((char *)packet, "AT+", 3) == 0){
            //ag_send_error = 1;
            log_info("Received not yet supported AT command\n");
        }
        hsp_run();
        return;
    }

    if (packet_type != HCI_EVENT_PACKET) return;
    uint8_t event = packet[0];
    bd_addr_t event_addr;
    uint16_t handle;

    switch (event) {
        case BTSTACK_EVENT_STATE:
            // bt stack activated, get started 
            if (packet[2] == HCI_STATE_WORKING){
                switch (hsp_role){
                    case HSP_AudioGateway:
                        hsp_ag_connect(remote);
                        break;
                    case HSP_Headset:
                        hsp_hs_connect(remote);
                        break;
                }
            }
            break;

        case HCI_EVENT_PIN_CODE_REQUEST:
            // inform about pin code request
            printf("Pin code request - using '0000'\n\r");
            bt_flip_addr(event_addr, &packet[2]);
            hci_send_cmd(&hci_pin_code_request_reply, &event_addr, 4, "0000");
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            handle = READ_BT_16(packet,3);
            if (handle == sco_handle){
                sco_handle = 0;
                hsp_state = HSP_W2_CONNECT_SCO;
            }
            break;
        case HCI_EVENT_SYNCHRONOUS_CONNECTION_COMPLETE:{
            int index = 2;
            uint8_t status = packet[index++];
            sco_handle = READ_BT_16(packet, index);
            index+=2;
            bd_addr_t address; 
            memcpy(address, &packet[index], 6);
            index+=6;
            uint8_t link_type = packet[index++];
            uint8_t transmission_interval = packet[index++];  // measured in slots
            uint8_t retransmission_interval = packet[index++];// measured in slots
            uint16_t rx_packet_length = READ_BT_16(packet, index); // measured in bytes
            index+=2;
            uint16_t tx_packet_length = READ_BT_16(packet, index); // measured in bytes
            index+=2;
            uint8_t air_mode = packet[index];

            if (status != 0){
                log_error("(e)SCO Connection is not established, status %u", status);
                exit(0);
                break;
            }
            switch (link_type){
                case 0x00:
                    printf("SCO Connection established. \n");
                    if (transmission_interval != 0) log_error("SCO Connection: transmission_interval not zero: %d.", transmission_interval);
                    if (retransmission_interval != 0) log_error("SCO Connection: retransmission_interval not zero: %d.", retransmission_interval);
                    if (rx_packet_length != 0) log_error("SCO Connection: rx_packet_length not zero: %d.", rx_packet_length);
                    if (tx_packet_length != 0) log_error("SCO Connection: tx_packet_length not zero: %d.", tx_packet_length);
                    break;
                case 0x02:
                    printf("eSCO Connection established. \n");
                    break;
                default:
                    log_error("(e)SCO reserved link_type 0x%2x", link_type);
                    break;
            }
            log_info("sco_handle 0x%2x, address %s, transmission_interval %u slots, retransmission_interval %u slots, " 
                 " rx_packet_length %u bytes, tx_packet_length %u bytes, air_mode 0x%2x (0x02 == CVSD)", sco_handle,
                 bd_addr_to_str(address), transmission_interval, retransmission_interval, rx_packet_length, tx_packet_length, air_mode);

            hsp_state = HSP_AG_ACTIVE;
            break;                
        }

        case RFCOMM_EVENT_INCOMING_CONNECTION:
            // data: event (8), len(8), address(48), channel (8), rfcomm_cid (16)
            if (hsp_state != HSP_IDLE) return;
            bt_flip_addr(event_addr, &packet[2]); 
            rfcomm_cid = READ_BT_16(packet, 9);
            printf("RFCOMM channel %u requested for %s\n", packet[8], bd_addr_to_str(event_addr));
            rfcomm_accept_connection_internal(rfcomm_cid);
            break;

        case RFCOMM_EVENT_OPEN_CHANNEL_COMPLETE:
            printf("RFCOMM_EVENT_OPEN_CHANNEL_COMPLETE packet_handler type %u, packet[0] %x\n", packet_type, packet[0]);
            // data: event(8), len(8), status (8), address (48), handle(16), server channel(8), rfcomm_cid(16), max frame size(16)
            if (packet[2]) {
                printf("RFCOMM channel open failed, status %u\n", packet[2]);
            } else {
                // data: event(8) , len(8), status (8), address (48), handle (16), server channel(8), rfcomm_cid(16), max frame size(16)
                rfcomm_handle = READ_BT_16(packet, 9);
                rfcomm_cid = READ_BT_16(packet, 12);
                mtu = READ_BT_16(packet, 14);
                printf("RFCOMM channel open succeeded. New RFCOMM Channel ID %u, max frame size %u\n", rfcomm_cid, mtu);
                hsp_state = HSP_W2_CONNECT_SCO;
                /** 
                 * @param handle
                 * @param transmit_bandwidth = 8000 (64kbps)
                 * @param receive_bandwidth = 8000 (64kbps)
                 * @param max_latency >= 7ms for eSCO, 0xFFFF do not care
                 * @param voice_settings = CVSD, Input Coding: Linear, Input Data Format: 2’s complement, data 16bit: 00011000000 == 0x60
                 * @param retransmission_effort = 0xFF do not care
                 * @param packet_type = at least EV3 for eSCO; here we have 0x3F = {HV1-3, EV3-5}
                 */
                break;
            }
            break;
        case DAEMON_EVENT_HCI_PACKET_SENT:
        case RFCOMM_EVENT_CREDITS:
            if (!rfcomm_cid) {
                switch (hsp_role){
                    case HSP_AudioGateway:
                        hsp_ag_init();
                        break;
                    case HSP_Headset:
                        hsp_hs_init();
                        break;
                }  
            }
            break;

        case RFCOMM_EVENT_CHANNEL_CLOSED:
            printf("RFCOMM channel closed\n");
            switch (hsp_role){
                case HSP_AudioGateway:
                    hsp_ag_init();
                    break;
                case HSP_Headset:
                    hsp_hs_init();
                    break;
            }
            break;
        default:
            break;
    }
    hsp_run();
}

void handle_query_rfcomm_event(sdp_query_event_t * event, void * context){
    sdp_query_rfcomm_service_event_t * ve;
    sdp_query_complete_event_t * ce;
            
    switch (event->type){
        case SDP_QUERY_RFCOMM_SERVICE:
            ve = (sdp_query_rfcomm_service_event_t*) event;
            channel_nr = ve->channel_nr;
            printf("** Service name: '%s', RFCOMM port %u\n", ve->service_name, channel_nr);
            break;
        case SDP_QUERY_COMPLETE:
            ce = (sdp_query_complete_event_t*) event;
            
            if (channel_nr > 0) {
                printf("RFCOMM create channel.\n");
                rfcomm_create_channel_internal(NULL, &remote, channel_nr); 
                break;
            }

            printf("Service not found, status %u.\n", ce->status);
            exit(0);
            break;
    }
}

void hsp_ag_create_service(uint8_t * service, int rfcomm_channel_nr, const char * name){
    uint8_t* attribute;
    de_create_sequence(service);

    // 0x0000 "Service Record Handle"
    de_add_number(service, DE_UINT, DE_SIZE_16, SDP_ServiceRecordHandle);
    de_add_number(service, DE_UINT, DE_SIZE_32, 0x10001);

    // 0x0001 "Service Class ID List"
    de_add_number(service,  DE_UINT, DE_SIZE_16, SDP_ServiceClassIDList);
    attribute = de_push_sequence(service);
    {
        //  "UUID for PAN Service"
        de_add_number(attribute, DE_UUID, DE_SIZE_16, SDP_Headset_AG);
        de_add_number(attribute, DE_UUID, DE_SIZE_16, SDP_GenericAudio);
    }
    de_pop_sequence(service, attribute);

    // 0x0004 "Protocol Descriptor List"
    de_add_number(service,  DE_UINT, DE_SIZE_16, SDP_ProtocolDescriptorList);
    attribute = de_push_sequence(service);
    {
        uint8_t* l2cpProtocol = de_push_sequence(attribute);
        {
            de_add_number(l2cpProtocol,  DE_UUID, DE_SIZE_16, SDP_L2CAPProtocol);
        }
        de_pop_sequence(attribute, l2cpProtocol);
        
        uint8_t* rfcomm = de_push_sequence(attribute);
        {
            de_add_number(rfcomm,  DE_UUID, DE_SIZE_16, SDP_RFCOMMProtocol);  // rfcomm_service
            de_add_number(rfcomm,  DE_UINT, DE_SIZE_8,  rfcomm_channel_nr);  // rfcomm channel
        }
        de_pop_sequence(attribute, rfcomm);
    }
    de_pop_sequence(service, attribute);

    // 0x0005 "Public Browse Group"
    de_add_number(service,  DE_UINT, DE_SIZE_16, SDP_BrowseGroupList); // public browse group
    attribute = de_push_sequence(service);
    {
        de_add_number(attribute,  DE_UUID, DE_SIZE_16, SDP_PublicBrowseGroup);
    }
    de_pop_sequence(service, attribute);

    // 0x0009 "Bluetooth Profile Descriptor List"
    de_add_number(service,  DE_UINT, DE_SIZE_16, SDP_BluetoothProfileDescriptorList);
    attribute = de_push_sequence(service);
    {
        uint8_t *sppProfile = de_push_sequence(attribute);
        {
            de_add_number(sppProfile,  DE_UUID, DE_SIZE_16, SDP_HSP); 
            de_add_number(sppProfile,  DE_UINT, DE_SIZE_16, 0x0102); // Verision 1.2
        }
        de_pop_sequence(attribute, sppProfile);
    }
    de_pop_sequence(service, attribute);

    // 0x0100 "Service Name"
    de_add_number(service,  DE_UINT, DE_SIZE_16, 0x0100);
    if (name){
        de_add_data(service,  DE_STRING, strlen(name), (uint8_t *) name);
    } else {
        de_add_data(service,  DE_STRING, strlen(default_hsp_ag_service_name), (uint8_t *) default_hsp_ag_service_name);
    }
}

void hsp_hs_create_service(uint8_t * service, int rfcomm_channel_nr, const char * name, uint8_t have_remote_audio_control){
    uint8_t* attribute;
    de_create_sequence(service);

    // 0x0000 "Service Record Handle"
    de_add_number(service, DE_UINT, DE_SIZE_16, SDP_ServiceRecordHandle);
    de_add_number(service, DE_UINT, DE_SIZE_32, 0x10001);

    // 0x0001 "Service Class ID List"
    de_add_number(service,  DE_UINT, DE_SIZE_16, SDP_ServiceClassIDList);
    attribute = de_push_sequence(service);
    {
        //  "UUID for PAN Service"
        de_add_number(attribute, DE_UUID, DE_SIZE_16, SDP_Headset_HS);
        de_add_number(attribute, DE_UUID, DE_SIZE_16, SDP_GenericAudio);
    }
    de_pop_sequence(service, attribute);

    // 0x0004 "Protocol Descriptor List"
    de_add_number(service,  DE_UINT, DE_SIZE_16, SDP_ProtocolDescriptorList);
    attribute = de_push_sequence(service);
    {
        uint8_t* l2cpProtocol = de_push_sequence(attribute);
        {
            de_add_number(l2cpProtocol,  DE_UUID, DE_SIZE_16, SDP_L2CAPProtocol);
        }
        de_pop_sequence(attribute, l2cpProtocol);
        
        uint8_t* rfcomm = de_push_sequence(attribute);
        {
            de_add_number(rfcomm,  DE_UUID, DE_SIZE_16, SDP_RFCOMMProtocol);  // rfcomm_service
            de_add_number(rfcomm,  DE_UINT, DE_SIZE_8,  rfcomm_channel_nr);  // rfcomm channel
        }
        de_pop_sequence(attribute, rfcomm);
    }
    de_pop_sequence(service, attribute);

    // 0x0005 "Public Browse Group"
    de_add_number(service,  DE_UINT, DE_SIZE_16, SDP_BrowseGroupList); // public browse group
    attribute = de_push_sequence(service);
    {
        de_add_number(attribute,  DE_UUID, DE_SIZE_16, SDP_PublicBrowseGroup);
    }
    de_pop_sequence(service, attribute);

    // 0x0009 "Bluetooth Profile Descriptor List"
    de_add_number(service,  DE_UINT, DE_SIZE_16, SDP_BluetoothProfileDescriptorList);
    attribute = de_push_sequence(service);
    {
        uint8_t *sppProfile = de_push_sequence(attribute);
        {
            de_add_number(sppProfile,  DE_UUID, DE_SIZE_16, SDP_HSP); 
            de_add_number(sppProfile,  DE_UINT, DE_SIZE_16, 0x0102); // Verision 1.2
        }
        de_pop_sequence(attribute, sppProfile);
    }
    de_pop_sequence(service, attribute);

    // 0x0100 "Service Name"
    de_add_number(service,  DE_UINT, DE_SIZE_16, 0x0100);
    if (name){
        de_add_data(service,  DE_STRING, strlen(name), (uint8_t *) name);
    } else {
        de_add_data(service,  DE_STRING, strlen(default_hsp_hs_service_name), (uint8_t *) default_hsp_hs_service_name);
    }
    
    // Remote audio volume control
    de_add_number(service, DE_UINT, DE_SIZE_16, 0x030C);
    de_add_number(service, DE_BOOL, DE_SIZE_8, have_remote_audio_control);
}

int btstack_main(int argc, const char * argv[]);
int btstack_main(int argc, const char * argv[]){

    printf("Client HCI init done\r\n");
        
    // init L2CAP
    l2cap_init();
    l2cap_register_packet_handler(packet_handler);

    rfcomm_init();
    rfcomm_register_packet_handler(packet_handler);
    rfcomm_register_service_internal(NULL, RFCOMM_SERVER_CHANNEL, 0xffff);  // reserved channel, mtu limited by l2cap

    hsp_role = HSP_AudioGateway;
    // init SDP, create record for SPP and register with SDP
    sdp_init();
    memset(hsp_service_buffer, 0, sizeof(hsp_service_buffer));
   
    switch(hsp_role){
        case HSP_AudioGateway:
            hsp_ag_create_service(hsp_service_buffer, RFCOMM_SERVER_CHANNEL, NULL);
            hsp_ag_init();
            break;
        case HSP_Headset:
            hsp_hs_create_service(hsp_service_buffer, RFCOMM_SERVER_CHANNEL, NULL, 0);
            hsp_hs_init();
            break;
    }
    
    sdp_register_service_internal(NULL, hsp_service_buffer);
    hsp_ag_init();

    sdp_query_rfcomm_register_callback(handle_query_rfcomm_event, NULL);

    // turn on!
    hci_power_control(HCI_POWER_ON);

    // go!
    run_loop_execute(); 
    return 0;
}
