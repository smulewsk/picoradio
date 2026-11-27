/*
 * Bluetooth A2DP sink (SBC) + AVRCP for the Pico 2 W radio.
 *
 * BTstack runs inside the CYW43 async_context task. Media packets are SBC
 * decoded right there and the PCM (stereo int16, 4 bytes per frame) is
 * written into g_radio.mp3_stream, which the decoder task plays when the
 * active source is Bluetooth.
 *
 * AVRCP: phone volume <-> our volume, track title/artist -> screen,
 * play/pause/next/prev from encoder 2.
 */
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "classic/btstack_sbc_bluedroid.h"
#include "pico/cyw43_arch.h"
#include "pico/async_context.h"

#include "bt_sink.h"
#include "radio.h"
#include "config.h"
#include "cfg.h"

#define MAX_SBC_PCM_FRAMES (16 * 8)          /* blocks * subbands */

static btstack_packet_callback_registration_t s_hci_cb;
static uint8_t s_sdp_a2dp[150];
static uint8_t s_sdp_avrcp_ct[200];
static uint8_t s_sdp_avrcp_tg[150];
static uint8_t s_sdp_devid[100];

static uint8_t s_sbc_caps[] = {
    0xFF,   /* all sampling frequencies, all channel modes */
    0xFF,   /* all block lengths, subbands, allocation methods */
    2, 53   /* bitpool range */
};
static uint8_t s_sbc_config[4];

static const btstack_sbc_decoder_t *s_sbc;
static btstack_sbc_decoder_bluedroid_t s_sbc_ctx;
static bool s_sbc_ready;

static volatile bool s_active;         /* BT is the selected source */
static uint16_t s_a2dp_cid, s_avrcp_cid;
static hci_con_handle_t s_con = HCI_CON_HANDLE_INVALID;
static bd_addr_t s_last_addr;
static bool s_have_last_addr;
static bool s_playing;
static uint16_t s_sample_rate = 44100;

static char s_title[96], s_artist[64];

/* ------------------------------------------------------------------ */

static void publish_title(void) {
    char t[160];
    if (s_artist[0] && s_title[0]) snprintf(t, sizeof t, "%s - %s", s_artist, s_title);
    else snprintf(t, sizeof t, "%s", s_title[0] ? s_title : s_artist);
    radio_set_title(t);
}

static void update_status(void) {
    if (!s_active) return;
    if (s_a2dp_cid == 0) {
        radio_set_status(RS_BT_WAIT);
        char m[48];
        snprintf(m, sizeof m, "Pair phone with '%s'", cfg()->bt_name);
        radio_set_info(m);
    } else if (s_playing) {
        radio_set_status(RS_BT_STREAMING);
        radio_set_info("");
    } else {
        radio_set_status(RS_BT_CONNECTED);
        radio_set_info("Connected - press play");
    }
}

/* ---------- audio path ---------- */

static void pcm_handler(int16_t *data, int num_frames, int num_channels, int sample_rate, void *ctx) {
    (void)ctx;
    if (!s_active || g_radio.flush_req) return;
    g_radio.pcm_sample_rate = (uint32_t)sample_rate;

    static int16_t stereo[MAX_SBC_PCM_FRAMES * 2];
    const int16_t *src = data;
    if (num_channels == 1) {
        if (num_frames > MAX_SBC_PCM_FRAMES) num_frames = MAX_SBC_PCM_FRAMES;
        for (int i = 0; i < num_frames; i++) stereo[2 * i] = stereo[2 * i + 1] = data[i];
        src = stereo;
    }
    size_t bytes = (size_t)num_frames * 4;
    /* write whole frames only, drop if the buffer is full */
    if (xStreamBufferSpacesAvailable(g_radio.mp3_stream) >= bytes) {
        xStreamBufferSend(g_radio.mp3_stream, src, bytes, 0);
    }
}

/* Reads the first SBC frame header of a packet for the status line:
 * sample rate, channel mode, bitpool -> real bitrate. */
static void sbc_stream_info(const uint8_t *h, uint16_t len) {
    if (len < 3 || h[0] != 0x9C) return;
    static const uint16_t FS[4] = { 16000, 32000, 44100, 48000 };
    static const uint8_t  BLK[4] = { 4, 8, 12, 16 };
    uint32_t fs = FS[h[1] >> 6];
    uint32_t blocks = BLK[(h[1] >> 4) & 3];
    uint32_t mode = (h[1] >> 2) & 3;                 /* 0 mono, 1 dual, 2 stereo, 3 joint */
    uint32_t subbands = (h[1] & 1) ? 8 : 4;
    uint32_t bitpool = h[2];
    uint32_t nch = mode == 0 ? 1 : 2;
    uint32_t bits;
    if (mode < 2)       bits = blocks * nch * bitpool;
    else if (mode == 2) bits = blocks * bitpool;
    else                bits = subbands + blocks * bitpool;
    uint32_t frame_len = 4 + (4 * subbands * nch) / 8 + (bits + 7) / 8;
    g_radio.codec = CODEC_SBC;
    g_radio.chmode = mode == 0 ? CH_MONO : mode == 1 ? CH_DUAL : mode == 2 ? CH_STEREO : CH_JOINT;
    g_radio.bitrate_kbps = (8 * frame_len * fs / (subbands * blocks) + 500) / 1000;
}

static void media_handler(uint8_t seid, uint8_t *packet, uint16_t size) {
    (void)seid;
    /* 12 byte RTP header, then 1 byte SBC header (frame count) */
    const int pos = 13;
    if (size <= pos || !s_sbc_ready) return;
    sbc_stream_info(packet + pos, (uint16_t)(size - pos));
    s_sbc->decode_signed_16(&s_sbc_ctx, 0, packet + pos, (uint16_t)(size - pos));
}

static void sbc_start(void) {
    s_sbc = btstack_sbc_decoder_bluedroid_init_instance(&s_sbc_ctx);
    s_sbc->configure(&s_sbc_ctx, SBC_MODE_STANDARD, pcm_handler, NULL);
    s_sbc_ready = true;
}

/* Deletes the stored key for a phone and checks that it's really gone: the
 * SDK's flash storage ignores flash_safe_execute() failures (no FreeRTOS heap
 * for its helper task), so a delete can silently not happen. */
static void drop_key(bd_addr_t a, const char *why) {
    gap_drop_link_key_for_bd_addr(a);
    link_key_t k;
    link_key_type_t t;
    bool still = gap_get_link_key_for_bd_addr(a, k, &t);
    printf("[bt] %s: key for %s %s (FreeRTOS heap free %u)\n", why, bd_addr_to_str(a),
           still ? "COULD NOT BE DELETED" : "deleted", (unsigned)xPortGetFreeHeapSize());
}

static int count_keys(void) {
    btstack_link_key_iterator_t it;
    bd_addr_t a;
    link_key_t k;
    link_key_type_t t;
    int n = 0;
    if (gap_link_key_iterator_init(&it)) {
        while (gap_link_key_iterator_get_next(&it, a, k, &t)) n++;
        gap_link_key_iterator_done(&it);
    }
    return n;
}

static uint32_t do_forget_all(void *p) {
    (void)p;
    if (s_con != HCI_CON_HANDLE_INVALID) gap_disconnect(s_con);
    gap_delete_all_link_keys();
    s_have_last_addr = false;
    int left = count_keys();
    printf("[bt] forget all pairings: %d key(s) left (FreeRTOS heap free %u)\n",
           left, (unsigned)xPortGetFreeHeapSize());
    return (uint32_t)left;
}

/* ---------- event handlers ---------- */

static void hci_handler(uint8_t type, uint16_t ch, uint8_t *packet, uint16_t size) {
    (void)ch; (void)size;
    if (type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            bd_addr_t a;
            gap_local_bd_addr(a);
            printf("[bt] up, address %s\n", bd_addr_to_str(a));
        }
        break;
    case HCI_EVENT_PIN_CODE_REQUEST: {
        bd_addr_t a;
        hci_event_pin_code_request_get_bd_addr(packet, a);
        gap_pin_code_response(a, "0000");
        break;
    }
    case HCI_EVENT_CONNECTION_COMPLETE:
        /* s_last_addr is only set once A2DP really connects: a phone that
         * merely tried (and failed) to pair must not be paged on reconnect */
        if (hci_event_connection_complete_get_status(packet) == 0)
            s_con = hci_event_connection_complete_get_connection_handle(packet);
        break;
    case GAP_EVENT_PAIRING_STARTED:
        printf("[bt] pairing started\n");
        if (s_active) radio_set_info("Pairing...");
        break;
    case GAP_EVENT_PAIRING_COMPLETE: {
        bd_addr_t a;
        gap_event_pairing_complete_get_bd_addr(packet, a);
        uint8_t st = gap_event_pairing_complete_get_status(packet);
        printf("[bt] pairing with %s: status 0x%02x%s%s%s\n", bd_addr_to_str(a), st,
               st == ERROR_CODE_AUTHENTICATION_FAILURE ? " (authentication failure: forget \"" : "",
               st == ERROR_CODE_AUTHENTICATION_FAILURE ? cfg()->bt_name : "",
               st == ERROR_CODE_AUTHENTICATION_FAILURE ? "\" on the phone and pair again)" : "");
        if (st != ERROR_CODE_SUCCESS) {
            /* forget any old key, so the next attempt starts clean */
            drop_key(a, "pairing failed");
            if (s_have_last_addr && bd_addr_cmp(a, s_last_addr) == 0) s_have_last_addr = false;
            if (s_active) {
                char m[40];
                snprintf(m, sizeof m, "Pairing failed (0x%02x) - retry", st);
                radio_set_info(m);
            }
        } else {
            update_status();
        }
        break;
    }
    case HCI_EVENT_AUTHENTICATION_COMPLETE: {
        uint8_t st = hci_event_authentication_complete_get_status(packet);
        if (st == ERROR_CODE_PIN_OR_KEY_MISSING || st == ERROR_CODE_AUTHENTICATION_FAILURE) {
            /* the phone forgot us: drop our stale key too */
            hci_connection_t *c = hci_connection_for_handle(
                hci_event_authentication_complete_get_connection_handle(packet));
            if (c) {
                drop_key(c->address, "stale pairing");
                if (s_have_last_addr && bd_addr_cmp(c->address, s_last_addr) == 0) s_have_last_addr = false;
            }
        }
        break;
    }
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        s_con = HCI_CON_HANDLE_INVALID;
        break;
    default:
        break;
    }
}

static void a2dp_handler(uint8_t type, uint16_t ch, uint8_t *packet, uint16_t size) {
    (void)ch; (void)size;
    if (type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) return;

    switch (packet[2]) {
    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION:
        s_sample_rate = a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(packet);
        g_radio.pcm_sample_rate = s_sample_rate;
        printf("[bt] SBC %u Hz\n", s_sample_rate);
        break;
    case A2DP_SUBEVENT_STREAM_ESTABLISHED:
        if (a2dp_subevent_stream_established_get_status(packet) != ERROR_CODE_SUCCESS) break;
        s_a2dp_cid = a2dp_subevent_stream_established_get_a2dp_cid(packet);
        a2dp_subevent_stream_established_get_bd_addr(packet, s_last_addr);
        s_have_last_addr = true;
        printf("[bt] A2DP connected to %s\n", bd_addr_to_str(s_last_addr));
        if (!s_active) {                     /* radio mode: refuse */
            a2dp_sink_disconnect(s_a2dp_cid);
            break;
        }
        update_status();
        break;
    case A2DP_SUBEVENT_STREAM_STARTED:
        sbc_start();
        s_playing = true;
        update_status();
        break;
    case A2DP_SUBEVENT_STREAM_SUSPENDED:
        s_playing = false;
        update_status();
        break;
    case A2DP_SUBEVENT_STREAM_RELEASED:
        s_playing = false;
        s_sbc_ready = false;
        update_status();
        break;
    case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
        s_a2dp_cid = 0;
        s_playing = false;
        s_sbc_ready = false;
        s_title[0] = s_artist[0] = 0;
        if (s_active) radio_set_title("");
        update_status();
        break;
    default:
        break;
    }
}

static void avrcp_handler(uint8_t type, uint16_t ch, uint8_t *packet, uint16_t size) {
    (void)ch; (void)size;
    if (type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    switch (packet[2]) {
    case AVRCP_SUBEVENT_CONNECTION_ESTABLISHED:
        if (avrcp_subevent_connection_established_get_status(packet) != ERROR_CODE_SUCCESS) {
            s_avrcp_cid = 0;
            break;
        }
        s_avrcp_cid = avrcp_subevent_connection_established_get_avrcp_cid(packet);
        avrcp_target_support_event(s_avrcp_cid, AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED);
        avrcp_controller_get_supported_events(s_avrcp_cid);
        break;
    case AVRCP_SUBEVENT_CONNECTION_RELEASED:
        s_avrcp_cid = 0;
        break;
    default:
        break;
    }
}

static void copy_value(char *dst, size_t dn, const uint8_t *v, uint16_t len) {
    if (len >= dn) len = (uint16_t)(dn - 1);
    memcpy(dst, v, len);
    dst[len] = 0;
}

static void avrcp_ct_handler(uint8_t type, uint16_t ch, uint8_t *packet, uint16_t size) {
    (void)ch; (void)size;
    if (type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    if (s_avrcp_cid == 0) return;
    switch (packet[2]) {
    case AVRCP_SUBEVENT_GET_CAPABILITY_EVENT_ID_DONE:
        avrcp_controller_enable_notification(s_avrcp_cid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_STATUS_CHANGED);
        avrcp_controller_enable_notification(s_avrcp_cid, AVRCP_NOTIFICATION_EVENT_TRACK_CHANGED);
        avrcp_controller_get_now_playing_info(s_avrcp_cid);
        break;
    case AVRCP_SUBEVENT_NOTIFICATION_TRACK_CHANGED:
        avrcp_controller_get_now_playing_info(s_avrcp_cid);
        break;
    case AVRCP_SUBEVENT_NOTIFICATION_PLAYBACK_STATUS_CHANGED:
        s_playing = avrcp_subevent_notification_playback_status_changed_get_play_status(packet)
                    == AVRCP_PLAYBACK_STATUS_PLAYING;
        update_status();
        break;
    case AVRCP_SUBEVENT_NOW_PLAYING_TITLE_INFO:
        copy_value(s_title, sizeof s_title,
                   avrcp_subevent_now_playing_title_info_get_value(packet),
                   avrcp_subevent_now_playing_title_info_get_value_len(packet));
        publish_title();
        break;
    case AVRCP_SUBEVENT_NOW_PLAYING_ARTIST_INFO:
        copy_value(s_artist, sizeof s_artist,
                   avrcp_subevent_now_playing_artist_info_get_value(packet),
                   avrcp_subevent_now_playing_artist_info_get_value_len(packet));
        publish_title();
        break;
    default:
        break;
    }
}

static void avrcp_tg_handler(uint8_t type, uint16_t ch, uint8_t *packet, uint16_t size) {
    (void)ch; (void)size;
    if (type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    if (packet[2] == AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED) {
        int abs_vol = avrcp_subevent_notification_volume_changed_get_absolute_volume(packet);
        radio_set_volume((abs_vol * VOLUME_MAX + 63) / 127);
    }
}

/* ---------- functions executed on the BTstack context ---------- */

static uint32_t do_init(void *p) {
    (void)p;
    l2cap_init();
    sdp_init();
    a2dp_sink_init();
    avrcp_init();
    avrcp_controller_init();
    avrcp_target_init();

    a2dp_sink_register_packet_handler(a2dp_handler);
    a2dp_sink_register_media_handler(media_handler);
    avdtp_stream_endpoint_t *ep = a2dp_sink_create_stream_endpoint(
        AVDTP_AUDIO, AVDTP_CODEC_SBC, s_sbc_caps, sizeof s_sbc_caps, s_sbc_config, sizeof s_sbc_config);
    btstack_assert(ep != NULL);
    (void)ep;

    avrcp_register_packet_handler(avrcp_handler);
    avrcp_controller_register_packet_handler(avrcp_ct_handler);
    avrcp_target_register_packet_handler(avrcp_tg_handler);

    a2dp_sink_create_sdp_record(s_sdp_a2dp, sdp_create_service_record_handle(),
                                AVDTP_SINK_FEATURE_MASK_SPEAKER, NULL, NULL);
    sdp_register_service(s_sdp_a2dp);
    avrcp_controller_create_sdp_record(s_sdp_avrcp_ct, sdp_create_service_record_handle(),
        1 << AVRCP_CONTROLLER_SUPPORTED_FEATURE_CATEGORY_PLAYER_OR_RECORDER, NULL, NULL);
    sdp_register_service(s_sdp_avrcp_ct);
    avrcp_target_create_sdp_record(s_sdp_avrcp_tg, sdp_create_service_record_handle(),
        1 << AVRCP_TARGET_SUPPORTED_FEATURE_CATEGORY_MONITOR_OR_AMPLIFIER, NULL, NULL);
    sdp_register_service(s_sdp_avrcp_tg);
    device_id_create_sdp_record(s_sdp_devid, sdp_create_service_record_handle(),
        DEVICE_ID_VENDOR_ID_SOURCE_BLUETOOTH, BLUETOOTH_COMPANY_ID_BLUEKITCHEN_GMBH, 1, 1);
    sdp_register_service(s_sdp_devid);

    gap_set_local_name(cfg()->bt_name);      /* BTstack keeps the pointer: cfg() storage is static */
    gap_set_class_of_device(0x240414);       /* audio, loudspeaker */
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    /* No screen or buttons to confirm with: accept the "just works" pairing
     * ourselves. BTstack leaves this off by default, and without it the chip
     * waits for an answer that never comes and fails with 0x05. */
    gap_ssp_set_auto_accept(1);
    /* BR/EDR Secure Connections (P-256) pairing fails on the CYW43439 with
     * many phones ("authentication failure", 0x05). Classic SSP (P-192) is
     * still encrypted and works everywhere. */
    gap_secure_connections_enable(BT_SECURE_CONNECTIONS);
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH | LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    gap_set_allow_role_switch(true);
    /* reconnecting to an absent phone blocks page scan (new pairings) while
     * it runs: give up after 2.5 s instead of the default 5 s */
    gap_set_page_timeout(0x1000);
    gap_discoverable_control(0);
    gap_connectable_control(0);

    s_hci_cb.callback = hci_handler;
    hci_add_event_handler(&s_hci_cb);
    hci_power_control(HCI_POWER_ON);
    return 0;
}

static uint32_t do_activate(void *p) {
    bool on = (bool)(uintptr_t)p;
    s_active = on;
    if (on) {
        gap_connectable_control(1);
        gap_discoverable_control(1);
        s_title[0] = s_artist[0] = 0;
        update_status();
        /* try to reconnect to the last phone (or the first paired one) */
        if (!s_have_last_addr) {
            btstack_link_key_iterator_t it;
            link_key_t key;
            link_key_type_t kt;
            if (gap_link_key_iterator_init(&it)) {
                if (gap_link_key_iterator_get_next(&it, s_last_addr, key, &kt)) s_have_last_addr = true;
                gap_link_key_iterator_done(&it);
            }
        }
        if (s_have_last_addr && s_a2dp_cid == 0) {
            uint16_t cid;
            printf("[bt] reconnecting to %s\n", bd_addr_to_str(s_last_addr));
            a2dp_sink_establish_stream(s_last_addr, &cid);
        }
    } else {
        gap_discoverable_control(0);
        gap_connectable_control(0);
        if (s_a2dp_cid && s_playing && s_avrcp_cid) avrcp_controller_pause(s_avrcp_cid);
        if (s_con != HCI_CON_HANDLE_INVALID) gap_disconnect(s_con);
        s_playing = false;
    }
    return 0;
}

static uint32_t do_cmd(void *p) {
    if (!s_avrcp_cid) return 0;
    switch ((int)(uintptr_t)p) {
    case 0:
        if (s_playing) avrcp_controller_pause(s_avrcp_cid);
        else avrcp_controller_play(s_avrcp_cid);
        break;
    case 1: avrcp_controller_forward(s_avrcp_cid); break;
    case 2: avrcp_controller_backward(s_avrcp_cid); break;
    }
    return 0;
}

static uint32_t do_volume(void *p) {
    if (s_avrcp_cid) avrcp_target_volume_changed(s_avrcp_cid, (uint8_t)(uintptr_t)p);
    return 0;
}

/* ---------- public API (any task) ---------- */

static void run(uint32_t (*fn)(void *), uintptr_t arg) {
    async_context_execute_sync(cyw43_arch_async_context(), fn, (void *)arg);
}

void bt_init(void)              { run(do_init, 0); }
void bt_activate(bool on)       { run(do_activate, on); }
void bt_play_pause(void)        { run(do_cmd, 0); }
void bt_next(void)              { run(do_cmd, 1); }
void bt_prev(void)              { run(do_cmd, 2); }
int  bt_forget_all(void) {
    return (int)async_context_execute_sync(cyw43_arch_async_context(), do_forget_all, NULL);
}
void bt_sync_volume(int volume) {
    if (!s_active) return;
    run(do_volume, (uintptr_t)(volume * 127 / VOLUME_MAX));
}
