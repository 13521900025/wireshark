/* blf.c
 *
 * Wiretap Library
 * Copyright (c) 1998 by Gilbert Ramirez <gram@alumni.rice.edu>
 *
 * File format support for the Binary Log File (BLF) file format from
 * Vector Informatik decoder
 * Copyright (c) 2021-2022 by Dr. Lars Voelker <lars.voelker@technica-engineering.de>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

 /*
  * The following was used as a reference for the file format:
  *     https://bitbucket.org/tobylorenz/vector_blf
  * The repo above includes multiple examples files as well.
  */

#include <config.h>
#define WS_LOG_DOMAIN LOG_DOMAIN_WIRETAP

#include "blf.h"

#include <epan/dissectors/packet-socketcan.h>
#include <string.h>
#include <errno.h>
#include <epan/value_string.h>
#include <wsutil/wslog.h>
#include <wsutil/exported_pdu_tlvs.h>
#include "file_wrappers.h"
#include "wtap-int.h"

#ifdef HAVE_ZLIB
#define ZLIB_CONST
#include <zlib.h>
#endif /* HAVE_ZLIB */

static const guint8 blf_magic[] = { 'L', 'O', 'G', 'G' };
static const guint8 blf_obj_magic[] = { 'L', 'O', 'B', 'J' };

static int blf_file_type_subtype = -1;

void register_blf(void);

static gboolean blf_read(wtap *wth, wtap_rec *rec, Buffer *buf, int *err, gchar **err_info, gint64 *data_offset);
static gboolean blf_seek_read(wtap *wth, gint64 seek_off, wtap_rec* rec, Buffer *buf, int *err, gchar **err_info);
static void blf_close(wtap *wth);

/*
 * The virtual buffer looks like this (skips all headers):
 * uncompressed log container data
 * uncompressed log container data
 * ...
 *
 * The "real" positions, length, etc. reference this layout and not the file.
 * When no compression is used the file is accessed directly.
 */

typedef struct blf_log_container {
    gint64   infile_start_pos;        /* start position of log container in file */
    guint64  infile_length;           /* length of log container in file */
    guint64  infile_data_start;       /* start position of data in log container in file */

    gint64   real_start_pos;          /* decompressed (virtual) start position including header */
    guint64  real_length;             /* decompressed length */
    gint64   real_first_object_pos;   /* where does the first obj start? */
    guint64  real_leftover_bytes;     /* how many bytes are left over for the next container? */

    guint16  compression_method;      /* 0: uncompressed, 2: zlib */

    unsigned char  *real_data;        /* cache for decompressed data */
} blf_log_container_t;

typedef struct blf_data {
    gint64  start_of_last_obj;
    gint64  current_real_seek_pos;
    guint64 start_offset_ns;

    guint   current_log_container;
    GArray *log_containers;

    GHashTable *channel_to_iface_ht;
    guint32     next_interface_id;
} blf_t;

typedef struct blf_params {
    wtap     *wth;
    wtap_rec *rec;
    Buffer   *buf;
    FILE_T    fh;

    blf_t    *blf_data;
} blf_params_t;

typedef struct blf_channel_to_iface_entry {
    int pkt_encap;
    guint16 channel;
    guint16 hwchannel;
    guint32 interface_id;
} blf_channel_to_iface_entry_t;

static void
blf_free_key(gpointer key) {
    g_free(key);
}

static void
blf_free_channel_to_iface_entry(gpointer data) {
     g_free(data);
}

static gint64
blf_calc_key_value(int pkt_encap, guint16 channel, guint16 hwchannel) {
    return ((gint64)pkt_encap << 32) | ((gint64)hwchannel << 16) | channel;
}

static void add_interface_name(wtap_block_t *int_data, int pkt_encap, guint16 channel, guint16 hwchannel, gchar *name) {
    if (name != NULL) {
        wtap_block_add_string_option_format(*int_data, OPT_IDB_NAME, "%s", name);
    } else {
        switch (pkt_encap) {
        case WTAP_ENCAP_ETHERNET:
            /* we use UINT16_MAX to encode no hwchannel */
            if (hwchannel == UINT16_MAX) {
                wtap_block_add_string_option_format(*int_data, OPT_IDB_NAME, "ETH-%u", channel);
            } else {
                wtap_block_add_string_option_format(*int_data, OPT_IDB_NAME, "ETH-%u-%u", channel, hwchannel);
            }
            break;
        case WTAP_ENCAP_IEEE_802_11:
            wtap_block_add_string_option_format(*int_data, OPT_IDB_NAME, "WLAN-%u", channel);
            break;
        case WTAP_ENCAP_FLEXRAY:
            wtap_block_add_string_option_format(*int_data, OPT_IDB_NAME, "FR-%u", channel);
            break;
        case WTAP_ENCAP_LIN:
            wtap_block_add_string_option_format(*int_data, OPT_IDB_NAME, "LIN-%u", channel);
            break;
        case WTAP_ENCAP_SOCKETCAN:
            wtap_block_add_string_option_format(*int_data, OPT_IDB_NAME, "CAN-%u", channel);
            break;
        default:
            wtap_block_add_string_option_format(*int_data, OPT_IDB_NAME, "ENCAP_%d-%u", pkt_encap, channel);
        }
    }
}

static guint32
blf_add_interface(blf_params_t *params, int pkt_encap, guint32 channel, guint16 hwchannel, gchar *name) {
    wtap_block_t int_data = wtap_block_create(WTAP_BLOCK_IF_ID_AND_INFO);
    wtapng_if_descr_mandatory_t *if_descr_mand = (wtapng_if_descr_mandatory_t*)wtap_block_get_mandatory_data(int_data);
    blf_channel_to_iface_entry_t *item = NULL;

    if_descr_mand->wtap_encap = pkt_encap;
    add_interface_name(&int_data, pkt_encap, channel, hwchannel, name);
    /*
     * The time stamp resolution in these files can be per-record;
     * the maximum resolution is nanoseconds, so we specify that
     * as the interface's resolution.
     *
     * We set the resolution for a record on a per-record basis,
     * based on what the record specifies.
     */
    if_descr_mand->time_units_per_second = 1000 * 1000 * 1000;
    if_descr_mand->tsprecision = WTAP_TSPREC_NSEC;
    wtap_block_add_uint8_option(int_data, OPT_IDB_TSRESOL, 9);
    if_descr_mand->snap_len = WTAP_MAX_PACKET_SIZE_STANDARD;
    if_descr_mand->num_stat_entries = 0;
    if_descr_mand->interface_statistics = NULL;
    wtap_add_idb(params->wth, int_data);

    if (params->wth->file_encap == WTAP_ENCAP_NONE) {
        params->wth->file_encap = if_descr_mand->wtap_encap;
    } else {
        if (params->wth->file_encap != if_descr_mand->wtap_encap) {
            params->wth->file_encap = WTAP_ENCAP_PER_PACKET;
        }
    }

    gint64 *key = NULL;
    key = g_new(gint64, 1);
    *key = blf_calc_key_value(pkt_encap, channel, hwchannel);

    item = g_new(blf_channel_to_iface_entry_t, 1);
    item->channel = channel;
    item->hwchannel = hwchannel;
    item->pkt_encap = pkt_encap;
    item->interface_id = params->blf_data->next_interface_id++;
    g_hash_table_insert(params->blf_data->channel_to_iface_ht, key, item);

    return item->interface_id;
}

static guint32
blf_lookup_interface(blf_params_t *params, int pkt_encap, guint16 channel, guint16 hwchannel, gchar *name) {
    gint64 key = blf_calc_key_value(pkt_encap, channel, hwchannel);
    blf_channel_to_iface_entry_t *item = NULL;

    if (params->blf_data->channel_to_iface_ht == NULL) {
        return 0;
    }

    item = (blf_channel_to_iface_entry_t *)g_hash_table_lookup(params->blf_data->channel_to_iface_ht, &key);

    if (item != NULL) {
        return item->interface_id;
    } else {
        return blf_add_interface(params, pkt_encap, channel, hwchannel, name);
    }
}

static void
fix_endianness_blf_date(blf_date_t *date) {
    date->year = GUINT16_FROM_LE(date->year);
    date->month = GUINT16_FROM_LE(date->month);
    date->dayofweek = GUINT16_FROM_LE(date->dayofweek);
    date->day = GUINT16_FROM_LE(date->day);
    date->hour = GUINT16_FROM_LE(date->hour);
    date->mins = GUINT16_FROM_LE(date->mins);
    date->sec = GUINT16_FROM_LE(date->sec);
    date->ms = GUINT16_FROM_LE(date->ms);
}

static void
fix_endianness_blf_fileheader(blf_fileheader_t *header) {
    header->header_length = GUINT32_FROM_LE(header->header_length);
    header->len_compressed = GUINT64_FROM_LE(header->len_compressed);
    header->len_uncompressed = GUINT64_FROM_LE(header->len_uncompressed);
    header->obj_count = GUINT32_FROM_LE(header->obj_count);
    header->obj_read = GUINT32_FROM_LE(header->obj_read);
    fix_endianness_blf_date(&(header->start_date));
    fix_endianness_blf_date(&(header->end_date));
    header->length3 = GUINT32_FROM_LE(header->length3);
}

static void
fix_endianness_blf_blockheader(blf_blockheader_t *header) {
    header->header_length = GUINT16_FROM_LE(header->header_length);
    header->header_type = GUINT16_FROM_LE(header->header_type);
    header->object_length = GUINT32_FROM_LE(header->object_length);
    header->object_type = GUINT32_FROM_LE(header->object_type);
}

static void
fix_endianness_blf_logcontainerheader(blf_logcontainerheader_t *header) {
    header->compression_method = GUINT16_FROM_LE(header->compression_method);
    header->res1 = GUINT16_FROM_LE(header->res1);
    header->res2 = GUINT32_FROM_LE(header->res2);
    header->uncompressed_size = GUINT32_FROM_LE(header->uncompressed_size);
    header->res4 = GUINT32_FROM_LE(header->res4);
}

static void
fix_endianness_blf_logobjectheader(blf_logobjectheader_t *header) {
    header->flags = GUINT32_FROM_LE(header->flags);
    header->client_index = GUINT16_FROM_LE(header->client_index);
    header->object_version = GUINT16_FROM_LE(header->object_version);
    header->object_timestamp = GUINT64_FROM_LE(header->object_timestamp);
}

static void
fix_endianness_blf_logobjectheader2(blf_logobjectheader2_t *header) {
    header->flags = GUINT32_FROM_LE(header->flags);
    header->object_version = GUINT16_FROM_LE(header->object_version);
    header->object_timestamp = GUINT64_FROM_LE(header->object_timestamp);
    header->original_timestamp = GUINT64_FROM_LE(header->object_timestamp);
}

static void
fix_endianness_blf_logobjectheader3(blf_logobjectheader3_t *header) {
    header->flags = GUINT32_FROM_LE(header->flags);
    header->static_size = GUINT16_FROM_LE(header->static_size);
    header->object_version = GUINT16_FROM_LE(header->object_version);
    header->object_timestamp = GUINT64_FROM_LE(header->object_timestamp);
}

static void
fix_endianness_blf_ethernetframeheader(blf_ethernetframeheader_t *header) {
    header->channel = GUINT16_FROM_LE(header->channel);
    header->direction = GUINT16_FROM_LE(header->direction);
    header->ethtype = GUINT16_FROM_LE(header->ethtype);
    header->tpid = GUINT16_FROM_LE(header->tpid);
    header->tci = GUINT16_FROM_LE(header->tci);
    header->payloadlength = GUINT16_FROM_LE(header->payloadlength);
}

static void
fix_endianness_blf_ethernetframeheader_ex(blf_ethernetframeheader_ex_t *header) {
    header->struct_length = GUINT16_FROM_LE(header->struct_length);
    header->flags = GUINT16_FROM_LE(header->flags);
    header->channel = GUINT16_FROM_LE(header->channel);
    header->hw_channel = GUINT16_FROM_LE(header->hw_channel);
    header->frame_duration = GUINT64_FROM_LE(header->frame_duration);
    header->frame_checksum = GUINT32_FROM_LE(header->frame_checksum);
    header->direction = GUINT16_FROM_LE(header->direction);
    header->frame_length = GUINT16_FROM_LE(header->frame_length);
    header->frame_handle = GUINT32_FROM_LE(header->frame_handle);
    header->error = GUINT32_FROM_LE(header->error);
}

static void
fix_endianness_blf_wlanframeheader(blf_wlanframeheader_t* header) {
    header->channel = GUINT16_FROM_LE(header->channel);
    header->flags = GUINT16_FROM_LE(header->flags);
    header->signal_strength = GUINT16_FROM_LE(header->signal_strength);
    header->signal_quality = GUINT16_FROM_LE(header->signal_quality);
    header->frame_length = GUINT16_FROM_LE(header->frame_length);
}

static void
fix_endianness_blf_canmessage(blf_canmessage_t *header) {
    header->channel = GUINT16_FROM_LE(header->channel);
    header->id = GUINT32_FROM_LE(header->id);
}

static void
fix_endianness_blf_canmessage2_trailer(blf_canmessage2_trailer_t *header) {
    header->frameLength_in_ns = GUINT32_FROM_LE(header->frameLength_in_ns);
    header->reserved2 = GUINT16_FROM_LE(header->reserved1);
}

static void
fix_endianness_blf_canfdmessage(blf_canfdmessage_t *header) {
    header->channel = GUINT16_FROM_LE(header->channel);
    header->id = GUINT32_FROM_LE(header->id);
    header->frameLength_in_ns = GUINT32_FROM_LE(header->frameLength_in_ns);
    header->reservedCanFdMessage2 = GUINT32_FROM_LE(header->reservedCanFdMessage2);
}

static void
fix_endianness_blf_canfdmessage64(blf_canfdmessage64_t *header) {
    header->id = GUINT32_FROM_LE(header->id);
    header->frameLength_in_ns = GUINT32_FROM_LE(header->frameLength_in_ns);
    header->flags = GUINT32_FROM_LE(header->flags);
    header->btrCfgArb = GUINT32_FROM_LE(header->btrCfgArb);
    header->btrCfgData = GUINT32_FROM_LE(header->btrCfgData);
    header->timeOffsetBrsNs = GUINT32_FROM_LE(header->timeOffsetBrsNs);
    header->timeOffsetCrcDelNs = GUINT32_FROM_LE(header->timeOffsetCrcDelNs);
    header->bitCount = GUINT16_FROM_LE(header->bitCount);
    header->crc = GUINT32_FROM_LE(header->crc);
}

static void
fix_endianness_blf_canerror(blf_canerror_t *header) {
    header->channel = GUINT16_FROM_LE(header->channel);
    header->length = GUINT16_FROM_LE(header->length);
}

static void
fix_endianness_blf_canerrorext(blf_canerrorext_t *header) {
    header->channel = GUINT16_FROM_LE(header->channel);
    header->length = GUINT16_FROM_LE(header->length);
    header->flags = GUINT32_FROM_LE(header->flags);
    header->frameLength_in_ns = GUINT32_FROM_LE(header->frameLength_in_ns);
    header->id = GUINT32_FROM_LE(header->id);
    header->errorCodeExt = GUINT16_FROM_LE(header->errorCodeExt);
}

static void
fix_endianness_blf_canfderror64(blf_canfderror64_t *header) {
    header->flags = GUINT16_FROM_LE(header->flags);
    header->errorCodeExt = GUINT16_FROM_LE(header->errorCodeExt);
    header->extFlags = GUINT16_FROM_LE(header->extFlags);
    header->id = GUINT32_FROM_LE(header->id);
    header->frameLength_in_ns = GUINT32_FROM_LE(header->frameLength_in_ns);
    header->btrCfgArb = GUINT32_FROM_LE(header->btrCfgArb);
    header->btrCfgData = GUINT32_FROM_LE(header->btrCfgData);
    header->timeOffsetBrsNs = GUINT32_FROM_LE(header->timeOffsetBrsNs);
    header->timeOffsetCrcDelNs = GUINT32_FROM_LE(header->timeOffsetCrcDelNs);
    header->crc = GUINT32_FROM_LE(header->crc);
    header->errorPosition = GUINT16_FROM_LE(header->errorPosition);
}

static void
fix_endianness_blf_flexraydata(blf_flexraydata_t *header) {
    header->channel = GUINT16_FROM_LE(header->channel);
    header->messageId = GUINT16_FROM_LE(header->messageId);
    header->crc = GUINT16_FROM_LE(header->crc);
    header->reservedFlexRayData2 = GUINT16_FROM_LE(header->reservedFlexRayData2);
}

static void
fix_endianness_blf_flexraymessage(blf_flexraymessage_t *header) {
    header->channel = GUINT16_FROM_LE(header->channel);
    header->fpgaTick = GUINT32_FROM_LE(header->fpgaTick);
    header->fpgaTickOverflow = GUINT32_FROM_LE(header->fpgaTickOverflow);
    header->clientIndexFlexRayV6Message = GUINT32_FROM_LE(header->clientIndexFlexRayV6Message);
    header->clusterTime = GUINT32_FROM_LE(header->clusterTime);
    header->frameId = GUINT16_FROM_LE(header->frameId);
    header->headerCrc = GUINT16_FROM_LE(header->headerCrc);
    header->frameState = GUINT16_FROM_LE(header->frameState);
    header->reservedFlexRayV6Message2 = GUINT16_FROM_LE(header->reservedFlexRayV6Message2);
}

static void
fix_endianness_blf_flexrayrcvmessage(blf_flexrayrcvmessage_t *header) {
    header->channel = GUINT16_FROM_LE(header->channel);
    header->version = GUINT16_FROM_LE(header->version);
    header->channelMask = GUINT16_FROM_LE(header->channelMask);
    header->dir = GUINT16_FROM_LE(header->dir);
    header->clientIndex = GUINT32_FROM_LE(header->clientIndex);
    header->clusterNo = GUINT32_FROM_LE(header->clusterNo);
    header->frameId = GUINT16_FROM_LE(header->frameId);
    header->headerCrc1 = GUINT16_FROM_LE(header->headerCrc1);
    header->headerCrc2 = GUINT16_FROM_LE(header->headerCrc2);
    header->payloadLength = GUINT16_FROM_LE(header->payloadLength);
    header->payloadLengthValid = GUINT16_FROM_LE(header->payloadLengthValid);
    header->cycle = GUINT16_FROM_LE(header->cycle);
    header->tag = GUINT32_FROM_LE(header->tag);
    header->data = GUINT32_FROM_LE(header->data);
    header->frameFlags = GUINT32_FROM_LE(header->frameFlags);
    header->appParameter = GUINT32_FROM_LE(header->appParameter);
/*  this would be extra for ext format:
    header->frameCRC = GUINT32_FROM_LE(header->frameCRC);
    header->frameLengthInNs = GUINT32_FROM_LE(header->frameLengthInNs);
    header->frameId1 = GUINT16_FROM_LE(header->frameId1);
    header->pduOffset = GUINT16_FROM_LE(header->pduOffset);
    header->blfLogMask = GUINT16_FROM_LE(header->blfLogMask);
*/
}

static void
fix_endianness_blf_linmessage(blf_linmessage_t* message) {
    message->channel = GUINT16_FROM_LE(message->channel);
    message->crc = GUINT16_FROM_LE(message->crc);
/*  skip the optional part
    message->res2 = GUINT32_FROM_LE(message->res2);
*/
}

static void
fix_endianness_blf_apptext_header(blf_apptext_t *header) {
    header->source = GUINT32_FROM_LE(header->source);
    header->reservedAppText1 = GUINT32_FROM_LE(header->reservedAppText1);
    header->textLength = GUINT32_FROM_LE(header->textLength);
    header->reservedAppText2 = GUINT32_FROM_LE(header->reservedAppText2);
}

static void
fix_endianness_blf_ethernet_status_header(blf_ethernet_status_t* header) {
    header->channel = GUINT16_FROM_LE(header->channel);
    header->flags = GUINT16_FROM_LE(header->flags);
    /*uint8_t linkStatus;*/
    /*uint8_t ethernetPhy;*/
    /*uint8_t duplex;*/
    /*uint8_t mdi;*/
    /*uint8_t connector;*/
    /*uint8_t clockMode;*/
    /*uint8_t pairs;*/
    /*uint8_t hardwareChannel;*/
    header->bitrate = GUINT32_FROM_LE(header->bitrate);
}

static void
blf_init_logcontainer(blf_log_container_t *tmp) {
    tmp->infile_start_pos = 0;
    tmp->infile_length = 0;
    tmp->infile_data_start = 0;
    tmp->real_start_pos = 0;
    tmp->real_length = 0;
    tmp->real_first_object_pos = -1;
    tmp->real_leftover_bytes = G_MAXUINT64;
    tmp->real_data = NULL;
    tmp->compression_method = 0;
}

static void
blf_add_logcontainer(blf_t *blf_data, blf_log_container_t log_container) {
    if (blf_data->log_containers == NULL) {
        blf_data->log_containers = g_array_sized_new(FALSE, FALSE, sizeof(blf_log_container_t), 1);
        blf_data->current_log_container = 0;
    } else {
        blf_data->current_log_container++;
    }

    g_array_append_val(blf_data->log_containers, log_container);
}

static gboolean
blf_get_logcontainer_by_index(blf_t *blf_data, guint container_index, blf_log_container_t **ret) {
    if (blf_data == NULL || blf_data->log_containers == NULL || container_index >= blf_data->log_containers->len) {
        return FALSE;
    }

    *ret = &g_array_index(blf_data->log_containers, blf_log_container_t, container_index);
    return TRUE;
}

static gboolean
blf_find_logcontainer_for_address(blf_t *blf_data, gint64 pos, blf_log_container_t **container, gint *container_index) {
    blf_log_container_t *tmp;

    if (blf_data == NULL || blf_data->log_containers == NULL) {
        return FALSE;
    }

    for (guint i = 0; i < blf_data->log_containers->len; i++) {
        tmp = &g_array_index(blf_data->log_containers, blf_log_container_t, i);
        if (tmp->real_start_pos <= pos && pos < tmp->real_start_pos + (gint64)tmp->real_length) {
            *container = tmp;
            *container_index = i;
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean
blf_pull_logcontainer_into_memory(blf_params_t *params, guint index_log_container, int *err, gchar **err_info) {
    blf_t *blf_data = params->blf_data;
    blf_log_container_t tmp;

    if (index_log_container >= blf_data->log_containers->len) {
        /*
         * XXX - does this represent a bug (WTAP_ERR_INTERNAL) or a
         * malformed file (WTAP_ERR_BAD_FILE)?
         */
        *err = WTAP_ERR_INTERNAL;
        *err_info = ws_strdup_printf("blf_pull_logcontainer_into_memory: index_log_container (%u) >= blf_data->log_containers->len (%u)",
                                     index_log_container, blf_data->log_containers->len);
        return FALSE;
    }

    tmp = g_array_index(blf_data->log_containers, blf_log_container_t, index_log_container);

    if (tmp.real_data != NULL) {
        return TRUE;
    }

    if (tmp.compression_method == BLF_COMPRESSION_ZLIB) {
#ifdef HAVE_ZLIB
        if (file_seek(params->fh, tmp.infile_data_start, SEEK_SET, err) == -1) {
            return FALSE;
        }

        /* pull compressed data into buffer */
        if (tmp.infile_start_pos < 0) {
            /*
             * XXX - does this represent a bug (WTAP_ERR_INTERNAL) or a
             * malformed file (WTAP_ERR_BAD_FILE)?
             */
            *err = WTAP_ERR_INTERNAL;
            *err_info = ws_strdup_printf("blf_pull_logcontainer_into_memory: tmp.infile_start_pos (%" G_GINT64_FORMAT ") < 0",
                                         tmp.infile_start_pos);
            return FALSE;
        }
        if (tmp.infile_data_start < (guint64)tmp.infile_start_pos) {
            /*
             * XXX - does this represent a bug (WTAP_ERR_INTERNAL) or a
             * malformed file (WTAP_ERR_BAD_FILE)?
             */
            *err = WTAP_ERR_INTERNAL;
            *err_info = ws_strdup_printf("blf_pull_logcontainer_into_memory: tmp.infile_data_start (%" G_GUINT64_FORMAT ") < tmp.infile_start_pos (%" G_GINT64_FORMAT ")",
                                         tmp.infile_data_start, tmp.infile_start_pos);
            return FALSE;
        }
        if (tmp.infile_length < tmp.infile_data_start - (guint64)tmp.infile_start_pos) {
            /*
             * XXX - does this represent a bug (WTAP_ERR_INTERNAL) or a
             * malformed file (WTAP_ERR_BAD_FILE)?
             */
            *err = WTAP_ERR_INTERNAL;
            *err_info = ws_strdup_printf("blf_pull_logcontainer_into_memory: tmp.infile_length (%" G_GUINT64_FORMAT ") < (tmp.infile_data_start (%" G_GUINT64_FORMAT ") - tmp.infile_start_pos (%" G_GINT64_FORMAT ")) = %" G_GUINT64_FORMAT,
                                         tmp.infile_length,
                                         tmp.infile_data_start, tmp.infile_start_pos,
                                         tmp.infile_data_start - (guint64)tmp.infile_start_pos);
            return FALSE;
        }
        guint64 data_length = tmp.infile_length - (tmp.infile_data_start - (guint64)tmp.infile_start_pos);
        if (data_length > UINT_MAX) {
            /*
             * XXX - does this represent a bug (WTAP_ERR_INTERNAL) or a
             * malformed file (WTAP_ERR_BAD_FILE)?
             */
            *err = WTAP_ERR_INTERNAL;
            *err_info = ws_strdup_printf("blf_pull_logcontainer_into_memory: data_length (%" G_GUINT64_FORMAT ") > UINT_MAX",
                                         data_length);
            return FALSE;
        }
        unsigned char *compressed_data = g_try_malloc0((gsize)tmp.infile_length);
        if (!wtap_read_bytes_or_eof(params->fh, compressed_data, (unsigned int)data_length, err, err_info)) {
            g_free(compressed_data);
            if (*err == WTAP_ERR_SHORT_READ) {
                /*
                 * XXX - our caller will turn this into an EOF.
                 * How *should* it be treated?
                 * For now, we turn it into Yet Another Internal Error,
                 * pending having better documentation of the file
                 * format.
                 */
                *err = WTAP_ERR_INTERNAL;
                *err_info = ws_strdup("blf_pull_logcontainer_into_memory: short read on compressed data");
            }
            return FALSE;
        }

        unsigned char *buf = g_try_malloc0((gsize)tmp.real_length);
        z_stream infstream = {0};

        infstream.avail_in  = (unsigned int)data_length;
        infstream.next_in   = compressed_data;
        infstream.avail_out = (unsigned int)tmp.real_length;
        infstream.next_out  = buf;

        /* the actual DE-compression work. */
        if (Z_OK != inflateInit(&infstream)) {
            /*
             * XXX - check the error code and handle this appropriately.
             */
            g_free(buf);
            g_free(compressed_data);
            *err = WTAP_ERR_INTERNAL;
            if (infstream.msg != NULL) {
                *err_info = ws_strdup_printf("blf_pull_logcontainer_into_memory: inflateInit failed for LogContainer %u, message\"%s\"",
                                              index_log_container,
                                              infstream.msg);
            } else {
                *err_info = ws_strdup_printf("blf_pull_logcontainer_into_memory: inflateInit failed for LogContainer %u",
                                              index_log_container);
            }
            ws_debug("inflateInit failed for LogContainer %u", index_log_container);
            if (infstream.msg != NULL) {
                ws_debug("inflateInit returned: \"%s\"", infstream.msg);
            }
            return FALSE;
        }

        int ret = inflate(&infstream, Z_NO_FLUSH);
        /* Z_OK should not happen here since we know how big the buffer should be */
        if (Z_STREAM_END != ret) {
            switch (ret) {

            case Z_NEED_DICT:
                *err = WTAP_ERR_DECOMPRESS;
                *err_info = ws_strdup("preset dictionary needed");
                break;

            case Z_STREAM_ERROR:
                *err = WTAP_ERR_DECOMPRESS;
                *err_info = (infstream.msg != NULL) ? ws_strdup(infstream.msg) : NULL;
                break;

            case Z_MEM_ERROR:
                /* This means "not enough memory". */
                *err = ENOMEM;
                *err_info = NULL;
                break;

            case Z_DATA_ERROR:
                /* This means "deflate stream invalid" */
                *err = WTAP_ERR_DECOMPRESS;
                *err_info = (infstream.msg != NULL) ? ws_strdup(infstream.msg) : NULL;
                break;

            case Z_BUF_ERROR:
                /* XXX - this is recoverable; what should we do here? */
                *err = WTAP_ERR_INTERNAL;
                *err_info = ws_strdup_printf("blf_pull_logcontainer_into_memory: Z_BUF_ERROR from inflate(), message \"%s\"",
                                             (infstream.msg != NULL) ? infstream.msg : "(none)");
                break;

            case Z_VERSION_ERROR:
                *err = WTAP_ERR_INTERNAL;
                *err_info = ws_strdup_printf("blf_pull_logcontainer_into_memory: Z_VERSION_ERROR from inflate(), message \"%s\"",
                                             (infstream.msg != NULL) ? infstream.msg : "(none)");
                break;

            default:
                *err = WTAP_ERR_INTERNAL;
                *err_info = ws_strdup_printf("blf_pull_logcontainer_into_memory: unexpected error %d from inflate(), message \"%s\"",
                                             ret,
                                             (infstream.msg != NULL) ? infstream.msg : "(none)");
                break;
            }
            g_free(buf);
            g_free(compressed_data);
            ws_debug("inflate failed (return code %d) for LogContainer %u", ret, index_log_container);
            if (infstream.msg != NULL) {
                ws_debug("inflate returned: \"%s\"", infstream.msg);
            }
            /* Free up any dynamically-allocated memory in infstream */
            inflateEnd(&infstream);
            return FALSE;
        }

        if (Z_OK != inflateEnd(&infstream)) {
            /*
             * The zlib manual says this only returns Z_OK on success
             * and Z_STREAM_ERROR if the stream state was inconsistent.
             *
             * It's not clear what useful information can be reported
             * for Z_STREAM_ERROR; a look at the 1.2.11 source indicates
             * that no string is returned to indicate what the problem
             * was.
             *
             * It's also not clear what to do about infstream if this
             * fails.
             */
            *err = WTAP_ERR_INTERNAL;
            *err_info = ws_strdup_printf("blf_pull_logcontainer_into_memory: inflateEnd failed for LogContainer %u", index_log_container);
            g_free(buf);
            g_free(compressed_data);
            ws_debug("inflateEnd failed for LogContainer %u", index_log_container);
            if (infstream.msg != NULL) {
                ws_debug("inflateEnd returned: \"%s\"", infstream.msg);
            }
            return FALSE;
        }

        g_free(compressed_data);
        tmp.real_data = buf;
        g_array_index(blf_data->log_containers, blf_log_container_t, index_log_container) = tmp;
        return TRUE;
#else
        *err = WTAP_ERR_DECOMPRESSION_NOT_SUPPORTED;
        *err_info = ws_strdup("blf_pull_logcontainer_into_memory: reading gzip-compressed containers isn't supported");
        return FALSE;
#endif
    }

    return FALSE;
}

static gboolean
blf_read_bytes_or_eof(blf_params_t *params, guint64 real_pos, void *target_buffer, unsigned int count, int *err, gchar **err_info) {
    blf_log_container_t *start_container;
    blf_log_container_t *end_container;
    blf_log_container_t *current_container;

    gint start_container_index = -1;
    gint end_container_index   = -1;
    gint current_container_index = -1;

    unsigned int copied = 0;
    unsigned int data_left;
    unsigned int start_in_buf;

    unsigned char *buf = (unsigned char *)target_buffer;

    if (!blf_find_logcontainer_for_address(params->blf_data, real_pos, &start_container, &start_container_index)) {
        /*
         * XXX - why is this treated as an EOF rather than an error?
         * *err appears to be 0, which means our caller treats it as an
         * EOF, at least when reading the log object header.
         */
        ws_debug("cannot read data because start position cannot be mapped");
        return FALSE;
    }
    if (!blf_find_logcontainer_for_address(params->blf_data, real_pos + count - 1, &end_container, &end_container_index)) {
        /*
         * XXX - why is this treated as an EOF rather than an error?
         * *err appears to be 0, which means our caller treats it as an
         * EOF, at least when reading the log object header.
         */
        ws_debug("cannot read data because end position cannot be mapped");
        return FALSE;
    }

    current_container_index = start_container_index;
    current_container = start_container;
    start_in_buf = (unsigned int)real_pos - (unsigned int)start_container->real_start_pos;

    switch (start_container->compression_method) {
    case BLF_COMPRESSION_NONE:
        while (current_container_index <= end_container_index) {
            if (!blf_get_logcontainer_by_index(params->blf_data, current_container_index, &current_container)) {
                /*
                 * XXX - does this represent a bug (WTAP_ERR_INTERNAL) or a
                 * malformed file (WTAP_ERR_BAD_FILE)?
                 */
                *err = WTAP_ERR_INTERNAL;
                *err_info = ws_strdup_printf("blf_read_bytes_or_eof: cannot refresh container");
                ws_debug("cannot refresh container");
                return FALSE;
            }

            data_left = (unsigned int)(current_container->real_length - start_in_buf);

            if (file_seek(params->fh, current_container->infile_data_start + start_in_buf, SEEK_SET, err) < 0) {
                ws_debug("cannot seek data");
                return FALSE;
            }

            if (data_left < (count - copied)) {
                if (!wtap_read_bytes_or_eof(params->fh, buf + copied, data_left, err, err_info)) {
                    ws_debug("cannot read data");
                    return FALSE;
                }
                copied += data_left;
                current_container_index++;
                start_in_buf = 0;
            } else {
                if (!wtap_read_bytes_or_eof(params->fh, buf + copied, count - copied, err, err_info)) {
                    ws_debug("cannot read data");
                    return FALSE;
                }
                return TRUE;
            }
        }
        break;

    case BLF_COMPRESSION_ZLIB:
        while (current_container_index <= end_container_index) {
            if (!blf_pull_logcontainer_into_memory(params, current_container_index, err, err_info)) {
                return FALSE;
            }

            if (!blf_get_logcontainer_by_index(params->blf_data, current_container_index, &current_container)) {
                /*
                 * XXX - does this represent a bug (WTAP_ERR_INTERNAL) or a
                 * malformed file (WTAP_ERR_BAD_FILE)?
                 */
                *err = WTAP_ERR_INTERNAL;
                *err_info = ws_strdup_printf("blf_read_bytes_or_eof: cannot refresh container");
                ws_debug("cannot refresh container");
                return FALSE;
            }

            if (current_container->real_data == NULL) {
                /*
                 * XXX - does this represent a bug (WTAP_ERR_INTERNAL) or a
                 * malformed file (WTAP_ERR_BAD_FILE)?
                 */
                *err = WTAP_ERR_INTERNAL;
                *err_info = ws_strdup_printf("blf_read_bytes_or_eof: pulling in container failed hard");
                ws_debug("pulling in container failed hard");
                return FALSE;
            }

            data_left = (unsigned int)(current_container->real_length - start_in_buf);

            if (data_left < (count - copied)) {
                memcpy(buf + copied, current_container->real_data + start_in_buf, (unsigned int)data_left);
                copied += data_left;
                current_container_index++;
                start_in_buf = 0;
            } else {
                memcpy(buf + copied, current_container->real_data + start_in_buf, count - copied);
                return TRUE;
            }
        }
        /*
         * XXX - does this represent a bug (WTAP_ERR_INTERNAL) or a
         * malformed file (WTAP_ERR_BAD_FILE)?
         */
        *err = WTAP_ERR_INTERNAL;
        *err_info = ws_strdup_printf("blf_read_bytes_or_eof: ran out of items in container");
        return FALSE;
        break;

    default:
        *err = WTAP_ERR_UNSUPPORTED;
        *err_info = ws_strdup_printf("blf: unknown compression method %u",
                                    start_container->compression_method);
        ws_debug("unknown compression method");
        return FALSE;
    }

    return FALSE;
}

static gboolean
blf_read_bytes(blf_params_t *params, guint64 real_pos, void *target_buffer, unsigned int count, int *err, gchar **err_info) {
    if (!blf_read_bytes_or_eof(params, real_pos, target_buffer, count, err, err_info)) {
        if (*err == 0) {
            *err = WTAP_ERR_SHORT_READ;
        }
        return FALSE;
    }
    return TRUE;
}

/* this is only called once on open to figure out the layout of the file */
static gboolean
blf_scan_file_for_logcontainers(blf_params_t *params) {
    blf_blockheader_t        header;
    blf_logcontainerheader_t logcontainer_header;
    blf_log_container_t      tmp;

    int    err;
    gchar *err_info;

    guint64  current_start_pos;
    guint64  current_real_start  = 0;

    while (1) {
        current_start_pos = file_tell(params->fh);

        /* Find Object */
        while (1) {
            if (!wtap_read_bytes_or_eof(params->fh, &header, sizeof header, &err, &err_info)) {
                ws_debug("we found end of file");

                /* lets ignore some bytes at the end since some implementations think it is ok to add a few zero bytes */
                if (err == WTAP_ERR_SHORT_READ) {
                    err = 0;
                }

                return TRUE;
            }

            fix_endianness_blf_blockheader(&header);

            if (memcmp(header.magic, blf_obj_magic, sizeof(blf_obj_magic))) {
                ws_debug("object magic is not LOBJ (pos: 0x%" PRIx64 ")", current_start_pos);
            } else {
                break;
            }

            /* we are moving back and try again but 1 byte later */
            /* TODO: better understand how this paddings works... */
            current_start_pos++;
            if (file_seek(params->fh, current_start_pos, SEEK_SET, &err) < 0) {
                return FALSE;
            }
        }

        if (header.header_type != BLF_HEADER_TYPE_DEFAULT) {
            ws_debug("unknown header type, I know only BLF_HEADER_TYPE_DEFAULT (1)");
            return FALSE;
        }

        switch (header.object_type) {
        case BLF_OBJTYPE_LOG_CONTAINER:
            if (header.header_length < sizeof(blf_blockheader_t)) {
                ws_debug("log container header length too short");
                return FALSE;
            }

            /* skip unknown header part if needed */
            if (header.header_length - sizeof(blf_blockheader_t) > 0) {
                /* seek over unknown header part */
                if (file_seek(params->fh, current_start_pos + header.header_length, SEEK_SET, &err) < 0) {
                    ws_debug("cannot seek file for skipping unknown header bytes in log container");
                    return FALSE;
                }
            }

            if (!wtap_read_bytes_or_eof(params->fh, &logcontainer_header, sizeof(blf_logcontainerheader_t), &err, &err_info)) {
                ws_debug("not enough bytes for log container header");
                return FALSE;
            }

            fix_endianness_blf_logcontainerheader(&logcontainer_header);

            blf_init_logcontainer(&tmp);

            tmp.infile_start_pos = current_start_pos;
            tmp.infile_data_start = file_tell(params->fh);
            tmp.infile_length = header.object_length;

            tmp.real_start_pos = current_real_start;
            tmp.real_length = logcontainer_header.uncompressed_size;
            tmp.compression_method = logcontainer_header.compression_method;

            /* set up next start position */
            current_real_start += logcontainer_header.uncompressed_size;

            if (file_seek(params->fh, current_start_pos + MAX(MAX(16, header.object_length), header.header_length), SEEK_SET, &err) < 0) {
                ws_debug("cannot seek file for skipping log container bytes");
                return FALSE;
            }

            blf_add_logcontainer(params->blf_data, tmp);

            break;
        default:
            ws_debug("we found a non BLF log container on top level. this is unexpected.");

            /* TODO: maybe create "fake Log Container" for this */
            if (file_seek(params->fh, current_start_pos + MAX(MAX(16, header.object_length), header.header_length), SEEK_SET, &err) < 0) {
                return FALSE;
            }
        }
    }

    return TRUE;
}

static void
blf_init_rec(blf_params_t *params, guint32 flags, guint64 object_timestamp, int pkt_encap, guint16 channel, guint16 hwchannel, guint caplen, guint len) {
    params->rec->rec_type = REC_TYPE_PACKET;
    params->rec->block = wtap_block_create(WTAP_BLOCK_PACKET);
    params->rec->presence_flags = WTAP_HAS_TS | WTAP_HAS_CAP_LEN | WTAP_HAS_INTERFACE_ID;
    switch (flags) {
    case BLF_TIMESTAMP_RESOLUTION_10US:
        params->rec->tsprec = WTAP_TSPREC_10_USEC;
        object_timestamp *= 10000;
        object_timestamp += params->blf_data->start_offset_ns;
        break;

    case BLF_TIMESTAMP_RESOLUTION_1NS:
        params->rec->tsprec = WTAP_TSPREC_NSEC;
        object_timestamp += params->blf_data->start_offset_ns;
        break;

    default:
        /*
         * XXX - report this as an error?
         *
         * Or provide a mechanism to allow file readers to report
         * a warning (an error that the reader tries to work
         * around and that the caller should report)?
         */
        ws_debug("I don't understand the flags 0x%x", flags);
        params->rec->tsprec = WTAP_TSPREC_NSEC;
        object_timestamp = 0;
        break;
    }
    params->rec->ts.secs = object_timestamp / (1000 * 1000 * 1000);
    params->rec->ts.nsecs = object_timestamp % (1000 * 1000 * 1000);
    params->rec->rec_header.packet_header.caplen = caplen;
    params->rec->rec_header.packet_header.len = len;

    nstime_t tmp_ts;
    tmp_ts.secs = params->blf_data->start_offset_ns / (1000 * 1000 * 1000);
    tmp_ts.nsecs = params->blf_data->start_offset_ns % (1000 * 1000 * 1000);
    nstime_delta(&params->rec->ts_rel_cap, &params->rec->ts, &tmp_ts);
    params->rec->ts_rel_cap_valid = true;

    params->rec->rec_header.packet_header.pkt_encap = pkt_encap;
    params->rec->rec_header.packet_header.interface_id = blf_lookup_interface(params, pkt_encap, channel, hwchannel, NULL);

    /* TODO: before we had to remove comments and verdict here to not leak memory but APIs have changed ... */
}

static void
blf_add_direction_option(blf_params_t *params, guint16 direction) {
    guint32 tmp = 0; /* dont care */

    switch (direction) {
    case BLF_DIR_RX:
        tmp = 1; /* inbound */
        break;
    case BLF_DIR_TX:
    case BLF_DIR_TX_RQ:
        tmp = 2; /* outbound */
        break;
    }

    /* pcapng.c: #define OPT_EPB_FLAGS 0x0002 */
    wtap_block_add_uint32_option(params->rec->block, 0x0002, tmp);
}

static gboolean
blf_read_log_object_header(blf_params_t *params, int *err, gchar **err_info, gint64 header2_start, gint64 data_start, blf_logobjectheader_t *logheader) {
    if (data_start - header2_start < (gint64)sizeof(blf_logobjectheader_t)) {
        *err = WTAP_ERR_BAD_FILE;
        *err_info = ws_strdup_printf("blf: not enough bytes for log object header");
        ws_debug("not enough bytes for timestamp header");
        return FALSE;
    }

    if (!blf_read_bytes_or_eof(params, header2_start, logheader, sizeof(*logheader), err, err_info)) {
        ws_debug("not enough bytes for logheader");
        return FALSE;
    }
    fix_endianness_blf_logobjectheader(logheader);
    return TRUE;
}

static gboolean
blf_read_log_object_header2(blf_params_t *params, int *err, gchar **err_info, gint64 header2_start, gint64 data_start, blf_logobjectheader2_t *logheader) {
    if (data_start - header2_start < (gint64)sizeof(blf_logobjectheader2_t)) {
        *err = WTAP_ERR_BAD_FILE;
        *err_info = ws_strdup_printf("blf: not enough bytes for log object header");
        ws_debug("not enough bytes for timestamp header");
        return FALSE;
    }

    if (!blf_read_bytes_or_eof(params, header2_start, logheader, sizeof(*logheader), err, err_info)) {
        ws_debug("not enough bytes for logheader");
        return FALSE;
    }
    fix_endianness_blf_logobjectheader2(logheader);
    return TRUE;
}

static gboolean
blf_read_log_object_header3(blf_params_t *params, int *err, gchar **err_info, gint64 header2_start, gint64 data_start, blf_logobjectheader3_t *logheader) {
    if (data_start - header2_start < (gint64)sizeof(blf_logobjectheader3_t)) {
        *err = WTAP_ERR_BAD_FILE;
        *err_info = ws_strdup_printf("blf: not enough bytes for log object header");
        ws_debug("not enough bytes for timestamp header");
        return FALSE;
    }

    if (!blf_read_bytes_or_eof(params, header2_start, logheader, sizeof(*logheader), err, err_info)) {
        ws_debug("not enough bytes for logheader");
        return FALSE;
    }
    fix_endianness_blf_logobjectheader3(logheader);
    return TRUE;
}

static gboolean
blf_read_ethernetframe(blf_params_t *params, int *err, gchar **err_info, gint64 block_start, gint64 data_start, gint64 object_length, guint32 flags, guint64 object_timestamp) {
    blf_ethernetframeheader_t ethheader;
    guint8 tmpbuf[18];
    guint caplen, len;

    if (object_length < (data_start - block_start) + (int) sizeof(blf_ethernetframeheader_t)) {
        *err = WTAP_ERR_BAD_FILE;
        *err_info = ws_strdup_printf("blf: ETHERNET_FRAME: not enough bytes for ethernet frame header in object");
        ws_debug("not enough bytes for ethernet frame header in object");
        return FALSE;
    }

    if (!blf_read_bytes(params, data_start, &ethheader, sizeof(ethheader), err, err_info)) {
        ws_debug("not enough bytes for ethernet frame header in file");
        return FALSE;
    }
    fix_endianness_blf_ethernetframeheader(&ethheader);

    /*
     * BLF breaks up and reorders the Ethernet header and VLAN tag fields.
     * This is a really bad design and makes this format one of the worst.
     * If you want a fast format that keeps your data intact, avoid this format!
     * So, lets hope we can reconstruct the original packet successfully.
     */

    tmpbuf[0] = ethheader.dst_addr[0];
    tmpbuf[1] = ethheader.dst_addr[1];
    tmpbuf[2] = ethheader.dst_addr[2];
    tmpbuf[3] = ethheader.dst_addr[3];
    tmpbuf[4] = ethheader.dst_addr[4];
    tmpbuf[5] = ethheader.dst_addr[5];
    tmpbuf[6] = ethheader.src_addr[0];
    tmpbuf[7] = ethheader.src_addr[1];
    tmpbuf[8] = ethheader.src_addr[2];
    tmpbuf[9] = ethheader.src_addr[3];
    tmpbuf[10] = ethheader.src_addr[4];
    tmpbuf[11] = ethheader.src_addr[5];

    if (ethheader.tpid != 0 && ethheader.tci != 0) {
        tmpbuf[12] = (ethheader.tpid & 0xff00) >> 8;
        tmpbuf[13] = (ethheader.tpid & 0x00ff);
        tmpbuf[14] = (ethheader.tci & 0xff00) >> 8;
        tmpbuf[15] = (ethheader.tci & 0x00ff);
        tmpbuf[16] = (ethheader.ethtype & 0xff00) >> 8;
        tmpbuf[17] = (ethheader.ethtype & 0x00ff);
        ws_buffer_assure_space(params->buf, (gsize)18 + ethheader.payloadlength);
        ws_buffer_append(params->buf, tmpbuf, (gsize)18);
        caplen = ((guint32)18 + ethheader.payloadlength);
        len = ((guint32)18 + ethheader.payloadlength);
    } else {
        tmpbuf[12] = (ethheader.ethtype & 0xff00) >> 8;
        tmpbuf[13] = (ethheader.ethtype & 0x00ff);
        ws_buffer_assure_space(params->buf, (gsize)14 + ethheader.payloadlength);
        ws_buffer_append(params->buf, tmpbuf, (gsize)14);
        caplen = ((guint32)14 + ethheader.payloadlength);
        len = ((guint32)14 + ethheader.payloadlength);
    }

    if (!blf_read_bytes(params, data_start + sizeof(blf_ethernetframeheader_t), ws_buffer_end_ptr(params->buf), ethheader.payloadlength, err, err_info)) {
        ws_debug("copying ethernet frame failed");
        return FALSE;
    }
    params->buf->first_free += ethheader.payloadlength;

    blf_init_rec(params, flags, object_timestamp, WTAP_ENCAP_ETHERNET, ethheader.channel, UINT16_MAX, caplen, len);
    blf_add_direction_option(params, ethheader.direction);

    return TRUE;
}

static gboolean
blf_read_ethernetframe_ext(blf_params_t *params, int *err, gchar **err_info, gint64 block_start, gint64 data_start, gint64 object_length, guint32 flags, guint64 object_timestamp) {
    blf_ethernetframeheader_ex_t ethheader;

    if (object_length < (data_start - block_start) + (int) sizeof(blf_ethernetframeheader_ex_t)) {
        *err = WTAP_ERR_BAD_FILE;
        *err_info = ws_strdup_printf("blf: ETHERNET_FRAME_EX: not enough bytes for ethernet frame header in object");
        ws_debug("not enough bytes for ethernet frame header in object");
        return FALSE;
    }

    if (!blf_read_bytes(params, data_start, &ethheader, sizeof(blf_ethernetframeheader_ex_t), err, err_info)) {
        ws_debug("not enough bytes for ethernet frame header in file");
        return FALSE;
    }
    fix_endianness_blf_ethernetframeheader_ex(&ethheader);

    ws_buffer_assure_space(params->buf, ethheader.frame_length);

    if (object_length - (data_start - block_start) - sizeof(blf_ethernetframeheader_ex_t) < ethheader.frame_length) {
        *err = WTAP_ERR_BAD_FILE;
        *err_info = ws_strdup_printf("blf: ETHERNET_FRAME_EX: frame too short");
        ws_debug("frame too short");
        return FALSE;
    }

    if (!blf_read_bytes(params, data_start + sizeof(blf_ethernetframeheader_ex_t), ws_buffer_start_ptr(params->buf), ethheader.frame_length, err, err_info)) {
        ws_debug("copying ethernet frame failed");
        return FALSE;
    }

    blf_init_rec(params, flags, object_timestamp, WTAP_ENCAP_ETHERNET, ethheader.channel, ethheader.hw_channel, ethheader.frame_length, ethheader.frame_length);
    wtap_block_add_uint32_option(params->rec->block, OPT_PKT_QUEUE, ethheader.hw_channel);
    blf_add_direction_option(params, ethheader.direction);

    return TRUE;
}

/*
 * XXX - provide radio information to our caller in the pseudo-header.
 */
static gboolean
blf_read_wlanframe(blf_params_t* params, int* err, gchar** err_info, gint64 block_start, gint64 data_start, gint64 object_length, guint32 flags, guint64 object_timestamp) {
    blf_wlanframeheader_t wlanheader;

    if (object_length < (data_start - block_start) + (int)sizeof(blf_wlanframeheader_t)) {
        *err = WTAP_ERR_BAD_FILE;
        *err_info = ws_strdup_printf("blf: WLAN_FRAME: not enough bytes for wlan frame header in object");
        ws_debug("not enough bytes for wlan frame header in object");
        return FALSE;
    }

    if (!blf_read_bytes(params, data_start, &wlanheader, sizeof(blf_wlanframeheader_t), err, err_info)) {
        ws_debug("not enough bytes for wlan frame header in file");
        return FALSE;
    }
    fix_endianness_blf_wlanframeheader(&wlanheader);

    ws_buffer_assure_space(params->buf, wlanheader.frame_length);

    if (object_length - (data_start - block_start) - sizeof(blf_wlanframeheader_t) < wlanheader.frame_length) {
        *err = WTAP_ERR_BAD_FILE;
        *err_info = ws_strdup_printf("blf: WLAN_FRAME: frame too short");
        ws_debug("frame too short");
        return FALSE;
    }

    if (!blf_read_bytes(params, data_start + sizeof(blf_wlanframeheader_t), ws_buffer_start_ptr(params->buf), wlanheader.frame_length, err, err_info)) {
        ws_debug("copying wlan frame failed");
        return FALSE;
    }

    blf_init_rec(params, flags, object_timestamp, WTAP_ENCAP_IEEE_802_11, wlanheader.channel, UINT16_MAX, wlanheader.frame_length, wlanheader.frame_length);
    blf_add_direction_option(params, wlanheader.direction);

    return TRUE;
}

static guint8 can_dlc_to_length[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 8, 8, 8, 8, 8, 8 };
static guint8 canfd_dlc_to_length[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 };

static gboolean
blf_can_fill_buf_and_rec(blf_params_t *params, int *err, gchar **err_info, guint32 canid, guint8 payload_length, guint8 payload_length_valid, guint64 start_position,
                         guint32 flags, guint64 object_timestamp, guint16 channel) {
    guint8   tmpbuf[8];
    guint    caplen, len;

    tmpbuf[0] = (canid & 0xff000000) >> 24;
    tmpbuf[1] = (canid & 0x00ff0000) >> 16;
    tmpbuf[2] = (canid & 0x0000ff00) >> 8;
    tmpbuf[3] = (canid & 0x000000ff);
    tmpbuf[4] = payload_length;
    tmpbuf[5] = 0;
    tmpbuf[6] = 0;
    tmpbuf[7] = 0;

    ws_buffer_assure_space(params->buf, sizeof(tmpbuf) + payload_length_valid);
    ws_buffer_append(params->buf, tmpbuf, sizeof(tmpbuf));
    caplen = sizeof(tmpbuf) + payload_length_valid;
    len = sizeof(tmpbuf) + payload_length;

    if (payload_length_valid > 0 && !blf_read_bytes(params, start_position, ws_buffer_end_ptr(params->buf), payload_length_valid, err, err_info)) {
        ws_debug("copying can payload failed");
        return FALSE;
    }
    params->buf->first_free += payload_length_valid;

    blf_init_rec(params, flags, object_timestamp, WTAP_ENCAP_SOCKETCAN, channel, UINT16_MAX, caplen, len);

    return TRUE;
}

static gboolean
blf_read_canmessage(blf_params_t *params, int *err, gchar **err_info, gint64 block_start, gint64 data_start, gint64 object_length, guint32 flags, guint64 object_timestamp, gboolean can_message2) {
    blf_canmessage_t canheader;
    blf_canmessage2_trailer_t can2trailer;

    guint32  canid;
    guint8   payload_length;

    if (object_length < (data_start - block_start) + (int) sizeof(canheader)) {
        *err = WTAP_ERR_BAD_FILE;
        *err_info = ws_strdup_printf("blf: %s: not enough bytes for can header in object",
                                    can_message2 ? "CAN_MESSAGE2" : "CAN_MESSAGE");
        ws_debug("not enough bytes for can header in object");
        return FALSE;
    }

    if (!blf_read_bytes(params, data_start, &canheader, sizeof(canheader), err, err_info)) {
        ws_debug("not enough bytes for can header in file");
        return FALSE;
    }
    fix_endianness_blf_canmessage(&canheader);

    canheader.dlc &= 0x0f;

    payload_length = canheader.dlc;
    if (payload_length > 8) {
        ws_debug("regular CAN tries more than 8 bytes? Cutting to 8!");
        payload_length = 8;
    }

    canid = canheader.id;

    if ((canheader.flags & BLF_CANMESSAGE_FLAG_RTR) == BLF_CANMESSAGE_FLAG_RTR) {
        canid |= CAN_RTR_FLAG;
        payload_length = 0;
    }

    if (!blf_can_fill_buf_and_rec(params, err, err_info, canid, payload_length, payload_length, data_start + sizeof(canheader), flags, object_timestamp, canheader.channel)) {
        return FALSE;
    }

    /* actually, we do not really need the data, right now.... */
    if (can_message2) {
        if (object_length < (data_start - block_start) + (int) sizeof(canheader) + 8 + (int) sizeof(can2trailer)) {
            *err = WTAP_ERR_BAD_FILE;
            *err_info = ws_strdup_printf("blf: CAN_MESSAGE2: not enough bytes for can message 2 trailer");
            ws_debug("not enough bytes for can message 2 trailer");
            return FALSE;
        }
        if (!blf_read_bytes(params, data_start + sizeof(canheader) + 8, &can2trailer, sizeof(can2trailer), err, err_info)) {
            ws_debug("not enough bytes for can message 2 trailer in file");
            return FALSE;
        }
        fix_endianness_blf_canmessage2_trailer(&can2trailer);
    }

    blf_add_direction_option(params, (canheader.flags & BLF_CANMESSAGE_FLAG_TX) == BLF_CANMESSAGE_FLAG_TX ? BLF_DIR_TX: BLF_DIR_RX);

    return TRUE;
}

static gboolean
blf_read_canfdmessage(blf_params_t *params, int *err, gchar **err_info, gint64 block_start, gint64 data_start, gint64 object_length, guint32 flags, guint64 object_timestamp) {
    blf_canfdmessage_t canheader;

    gboolean canfd;
    guint32  canid;
    guint8   payload_length;
    guint8   payload_length_valid;

    if (object_length < (data_start - block_start) + (int) sizeof(canheader)) {
        *err = WTAP_ERR_BAD_FILE;
        *err_info = ws_strdup_printf("blf: CAN_FD_MESSAGE: not enough bytes for canfd header in object");
        ws_debug("not enough bytes for canfd header in object");
        return FALSE;
    }

    if (!blf_read_bytes(params, data_start, &canheader, sizeof(canheader), err, err_info)) {
        ws_debug("not enough bytes for canfd header in file");
        return FALSE;
    }
    fix_endianness_blf_canfdmessage(&canheader);

    canheader.dlc &= 0x0f;

    canfd = (canheader.canfdflags & BLF_CANFDMESSAGE_CANFDFLAG_EDL) == BLF_CANFDMESSAGE_CANFDFLAG_EDL;
    if (canfd) {
        payload_length = canfd_dlc_to_length[canheader.dlc];
    } else {
        if (canheader.dlc > 8) {
            ws_debug("regular CAN tries more than 8 bytes?");
        }
        payload_length = can_dlc_to_length[canheader.dlc];
    }

    if (payload_length > canheader.validDataBytes) {
        ws_debug("shortening canfd payload because valid data bytes shorter!");
        payload_length = canheader.validDataBytes;
    }

    canid = canheader.id;

    if (!canfd && (canheader.flags & BLF_CANMESSAGE_FLAG_RTR) == BLF_CANMESSAGE_FLAG_RTR) {
        canid |= CAN_RTR_FLAG;
        payload_length = 0; /* Should already be zero from validDataBytes */
    }

    payload_length_valid = payload_length;

    if (payload_length_valid > object_length - (data_start - block_start) + sizeof(canheader)) {
        ws_debug("shortening can payload because buffer is too short!");
        payload_length_valid = (guint8)(object_length - (data_start - block_start));
    }

    if (!blf_can_fill_buf_and_rec(params, err, err_info, canid, payload_length, payload_length_valid, data_start + sizeof(canheader), flags, object_timestamp, canheader.channel)) {
        return FALSE;
    }

    blf_add_direction_option(params, (canheader.flags & BLF_CANMESSAGE_FLAG_TX) == BLF_CANMESSAGE_FLAG_TX ? BLF_DIR_TX : BLF_DIR_RX);

    return TRUE;
}

static gboolean
blf_read_canfdmessage64(blf_params_t *params, int *err, gchar **err_info, gint64 block_start, gint64 data_start, gint64 object_length, guint32 flags, guint64 object_timestamp) {
    blf_canfdmessage64_t canheader;

    gboolean canfd;
    guint32  canid;
    guint8   payload_length;
    guint8   payload_length_valid;

    if (object_length < (data_start - block_start) + (int) sizeof(canheader)) {
        *err = WTAP_ERR_BAD_FILE;
        *err_info = ws_strdup_printf("blf: CAN_FD_MESSAGE_64: not enough bytes for canfd header in object");
        ws_debug("not enough bytes for canfd header in object");
        return FALSE;
    }

    if (!blf_read_bytes(params, data_start, &canheader, sizeof(canheader), err, err_info)) {
        ws_debug("not enough bytes for canfd header in file");
        return FALSE;
    }
    fix_endianness_blf_canfdmessage64(&canheader);

    canheader.dlc &= 0x0f;

    canfd = (canheader.flags & BLF_CANFDMESSAGE64_FLAG_EDL) == BLF_CANFDMESSAGE64_FLAG_EDL;
    if (canfd) {
        payload_length = canfd_dlc_to_length[canheader.dlc];
    } else {
        if (canheader.dlc > 8) {
            ws_debug("regular CAN tries more than 8 bytes?");
        }
        payload_length = can_dlc_to_length[canheader.dlc];
    }

    if (payload_length > canheader.validDataBytes) {
        ws_debug("shortening canfd payload because valid data bytes shorter!");
        payload_length = canheader.validDataBytes;
    }

    canid = canheader.id;

    if (!canfd && (canheader.flags & BLF_CANFDMESSAGE64_FLAG_REMOTE_FRAME) == BLF_CANFDMESSAGE64_FLAG_REMOTE_FRAME) {
        canid |= CAN_RTR_FLAG;
        payload_length = 0; /* Should already be zero from validDataBytes */
    }

    payload_length_valid = payload_length;

    if (payload_length_valid > object_length - (data_start - block_start)) {
        ws_debug("shortening can payload because buffer is too short!");
        payload_length_valid = (guint8)(object_length - (data_start - block_start));
    }

    if (!blf_can_fill_buf_and_rec(params, err, err_info, canid, payload_length, payload_length_valid, data_start + sizeof(canheader), flags, object_timestamp, canheader.channel)) {
        return FALSE;
    }

    blf_add_direction_option(params, canheader.dir);

    return TRUE;
}

static gboolean
blf_read_canerror(blf_params_t *params, int *err, gchar **err_info, gint64 block_start, gint64 data_start, gint64 object_length, guint32 flags, guint64 object_timestamp) {
    blf_canerror_t canheader;
    guint32  canid;
    guint8   payload_length;
    guint8   tmpbuf[16] = {0};

    if (object_length < (data_start - block_start) + (int) sizeof(canheader)) {
        *err = WTAP_ERR_BAD_FILE;
        *err_info = ws_strdup_printf("blf: CAN_ERROR: not enough bytes for canerror header in object");
        ws_debug("not enough bytes for canerror header in object");
        return FALSE;
    }

    if (!blf_read_bytes(params, data_start, &canheader, sizeof(canheader), err, err_info)) {
        ws_debug("not enough bytes for canerror header in file");
        return FALSE;
    }
    fix_endianness_blf_canerror(&canheader);

    // Set CAN_ERR_FLAG in unused bits of Can ID to indicate error in socketcan
    canid = CAN_ERR_FLAG;

    // Fixed packet data length for socketcan error messages
    payload_length = CAN_ERR_DLC;

    tmpbuf[0] = (canid & 0xff000000) >> 24;
    tmpbuf[1] = (canid & 0x00ff0000) >> 16;
    tmpbuf[2] = (canid & 0x0000ff00) >> 8;
    tmpbuf[3] = (canid & 0x000000ff);
    tmpbuf[4] = payload_length;

    ws_buffer_assure_space(params->buf, sizeof(tmpbuf));
    ws_buffer_append(params->buf, tmpbuf, sizeof(tmpbuf));

    blf_init_rec(params, flags, object_timestamp, WTAP_ENCAP_SOCKETCAN, canheader.channel, UINT16_MAX, sizeof(tmpbuf), sizeof(tmpbuf));
    return TRUE;
}

static gboolean
blf_read_canerrorext(blf_params_t *params, int *err, gchar **err_info, gint64 block_start, gint64 data_start, gint64 object_length, guint32 flags, guint64 object_timestamp) {
    blf_canerrorext_t canheader;

    gboolean err_ack = false;
    gboolean err_prot = false;
    gboolean direction_tx;
    guint32  canid;
    guint8   payload_length;
    guint8   tmpbuf[16] = {0};

    if (object_length < (data_start - block_start) + (int) sizeof(canheader)) {
        *err = WTAP_ERR_BAD_FILE;
        *err_info = ws_strdup_printf("blf: CAN_ERROR_EXT: not enough bytes for canerrorext header in object");
        ws_debug("not enough bytes for canerrorext header in object");
        return FALSE;
    }

    if (!blf_read_bytes(params, data_start, &canheader, sizeof(canheader), err, err_info)) {
        ws_debug("not enough bytes for canerrorext header in file");
        return FALSE;
    }
    fix_endianness_blf_canerrorext(&canheader);

    if (canheader.flags & BLF_CANERROREXT_FLAG_CANCORE) {
        // Map Vector Can Core error codes to compareable socketcan errors
        switch ((canheader.errorCodeExt >> 6) & 0x3f) {
        case BLF_CANERROREXT_ECC_MEANING_BIT_ERROR:
            err_prot = true;
            tmpbuf[10] = CAN_ERR_PROT_BIT;
            break;
        case BLF_CANERROREXT_ECC_MEANING_FORM_ERROR:
            err_prot = true;
            tmpbuf[10] = CAN_ERR_PROT_FORM;
            break;
        case BLF_CANERROREXT_ECC_MEANING_STUFF_ERROR:
            err_prot = true;
            tmpbuf[10] = CAN_ERR_PROT_STUFF;
            break;
        case BLF_CANERROREXT_ECC_MEANING_CRC_ERROR:
            err_prot = true;
            tmpbuf[11] = CAN_ERR_PROT_LOC_CRC_SEQ;
            break;
        case BLF_CANERROREXT_ECC_MEANING_NACK_ERROR:
            err_ack = true;
            tmpbuf[11] = CAN_ERR_PROT_LOC_ACK;
            break;
        case BLF_CANERROREXT_ECC_MEANING_OVERLOAD:
            err_prot = true;
            tmpbuf[10] = CAN_ERR_PROT_OVERLOAD;
            break;
        default:
            err_prot = true;
            tmpbuf[10] = CAN_ERR_PROT_UNSPEC;
            break;
        }
        err_ack = err_ack || (canheader.errorCodeExt & BLF_CANERROREXT_EXTECC_NOT_ACK) == 0x0;
        if (err_ack) {
            // Don't set protocol error on ack errors
            err_prot = false;
        }
    }

    // CanID contains error class in socketcan
    canid = CAN_ERR_FLAG;
    canid |= err_prot ? CAN_ERR_PROT : 0;
    canid |= err_ack ? CAN_ERR_ACK : 0;

    // Fixed packet data length for socketcan error messages
    payload_length = CAN_ERR_DLC;
    canheader.dlc = payload_length;

    tmpbuf[0] = (canid & 0xff000000) >> 24;
    tmpbuf[1] = (canid & 0x00ff0000) >> 16;
    tmpbuf[2] = (canid & 0x0000ff00) >> 8;
    tmpbuf[3] = (canid & 0x000000ff);
    tmpbuf[4] = payload_length;

    ws_buffer_assure_space(params->buf, sizeof(tmpbuf));
    ws_buffer_append(params->buf, tmpbuf, sizeof(tmpbuf));

    blf_init_rec(params, flags, object_timestamp, WTAP_ENCAP_SOCKETCAN, canheader.channel, UINT16_MAX, sizeof(tmpbuf), sizeof(tmpbuf));
    if (canheader.flags & BLF_CANERROREXT_FLAG_CANCORE) {
        direction_tx = (canheader.errorCodeExt & BLF_CANERROREXT_EXTECC_TX) == BLF_CANERROREXT_EXTECC_TX;
        blf_add_direction_option(params, direction_tx ? BLF_DIR_TX: BLF_DIR_RX);
    }
    return TRUE;
}

static gboolean
blf_read_canfderror64(blf_params_t *params, int *err, gchar **err_info, gint64 block_start, gint64 data_start, gint64 object_length, guint32 flags, guint64 object_timestamp) {
    blf_canfderror64_t canheader;

    gboolean err_ack = false;
    gboolean err_prot = false;
    gboolean direction_tx;
    guint32  canid;
    guint8   payload_length;
    guint8   tmpbuf[16] = {0};

    if (object_length < (data_start - block_start) + (int) sizeof(canheader)) {
        *err = WTAP_ERR_BAD_FILE;
        *err_info = ws_strdup_printf("blf: CAN_FD_ERROR_64: not enough bytes for canfderror header in object");
        ws_debug("not enough bytes for canfderror header in object");
        return FALSE;
    }

    if (!blf_read_bytes(params, data_start, &canheader, sizeof(canheader), err, err_info)) {
        ws_debug("not enough bytes for canfderror header in file");
        return FALSE;
    }
    fix_endianness_blf_canfderror64(&canheader);

    if (canheader.flags & BLF_CANERROREXT_FLAG_CANCORE) {
        // Map Vector Can Core error codes to compareable socketcan errors
        switch ((canheader.errorCodeExt >> 6) & 0x3f) {
        case BLF_CANERROREXT_ECC_MEANING_BIT_ERROR:
            err_prot = true;
            tmpbuf[10] = CAN_ERR_PROT_BIT;
            break;
        case BLF_CANERROREXT_ECC_MEANING_FORM_ERROR:
            err_prot = true;
            tmpbuf[10] = CAN_ERR_PROT_FORM;
            break;
        case BLF_CANERROREXT_ECC_MEANING_STUFF_ERROR:
            err_prot = true;
            tmpbuf[10] = CAN_ERR_PROT_STUFF;
            break;
        case BLF_CANERROREXT_ECC_MEANING_CRC_ERROR:
            err_prot = true;
            tmpbuf[11] = CAN_ERR_PROT_LOC_CRC_SEQ;
            break;
        case BLF_CANERROREXT_ECC_MEANING_NACK_ERROR:
            err_ack = true;
            tmpbuf[11] = CAN_ERR_PROT_LOC_ACK;
            break;
        case BLF_CANERROREXT_ECC_MEANING_OVERLOAD:
            err_prot = true;
            tmpbuf[10] = CAN_ERR_PROT_OVERLOAD;
            break;
        default:
            err_prot = true;
            tmpbuf[10] = CAN_ERR_PROT_UNSPEC;
            break;
        }
        err_ack = err_ack || (canheader.errorCodeExt & BLF_CANERROREXT_EXTECC_NOT_ACK) == 0x0;
        if (err_ack) {
            // Don't set protocol error on ack errors
            err_prot = false;
        }
    }

    // CanID contains error class in socketcan
    canid = CAN_ERR_FLAG;
    canid |= err_prot ? CAN_ERR_PROT : 0;
    canid |= err_ack ? CAN_ERR_ACK : 0;

    // Fixed packet data length for socketcan error messages
    payload_length = CAN_ERR_DLC;
    canheader.dlc = payload_length;

    tmpbuf[0] = (canid & 0xff000000) >> 24;
    tmpbuf[1] = (canid & 0x00ff0000) >> 16;
    tmpbuf[2] = (canid & 0x0000ff00) >> 8;
    tmpbuf[3] = (canid & 0x000000ff);
    tmpbuf[4] = payload_length;

    ws_buffer_assure_space(params->buf, sizeof(tmpbuf));
    ws_buffer_append(params->buf, tmpbuf, sizeof(tmpbuf));

    blf_init_rec(params, flags, object_timestamp, WTAP_ENCAP_SOCKETCAN, canheader.channel, UINT16_MAX, sizeof(tmpbuf), sizeof(tmpbuf));
    if (canheader.flags & BLF_CANERROREXT_FLAG_CANCORE) {
        direction_tx = (canheader.errorCodeExt & BLF_CANERROREXT_EXTECC_TX) == BLF_CANERROREXT_EXTECC_TX;
        blf_add_direction_option(params, direction_tx ? BLF_DIR_TX: BLF_DIR_RX);
    }
    return TRUE;
}

static gboolean
blf_read_flexraydata(blf_params_t *params, int *err, gchar **err_info, gint64 block_start, gint64 data_start, gint64 object_length, guint32 flags, guint64 object_timestamp) {
    blf_flexraydata_t frheader;

    guint8 payload_length;
    guint8 payload_length_valid;
    guint8 tmpbuf[7];
    guint  caplen, len;

    if (object_length < (data_start - block_start) + (int) sizeof(frheader)) {
        *err = WTAP_ERR_BAD_FILE;
        *err_info = ws_strdup_printf("blf: FLEXRAY_DATA: not enough bytes for flexrayheader in object");
        ws_debug("not enough bytes for flexrayheader in object");
        return FALSE;
    }

    if (!blf_read_bytes(params, data_start, &frheader, sizeof(frheader), err, err_info)) {
        ws_debug("not enough bytes for flexrayheader header in file");
        return FALSE;
    }
    fix_endianness_blf_flexraydata(&frheader);

    payload_length = frheader.len;
    payload_length_valid = payload_length;

    if ((frheader.len & 0x01) == 0x01) {
        ws_debug("reading odd length in FlexRay!?");
    }

    if (payload_length_valid > object_length - (data_start - block_start) - sizeof(frheader)) {
        ws_debug("shortening FlexRay payload because buffer is too short!");
        payload_length_valid = (guint8)(object_length - (data_start - block_start) - sizeof(frheader));
    }

    if (frheader.channel != 0 && frheader.channel != 1) {
        ws_debug("FlexRay supports only two channels.");
    }

    /* Measurement Header */
    if (frheader.channel == 0) {
        tmpbuf[0] = BLF_FLEXRAYDATA_FRAME;
    } else {
        tmpbuf[0] = BLF_FLEXRAYDATA_FRAME | BLF_FLEXRAYDATA_CHANNEL_B;
    }

    /* Error Flags */
    tmpbuf[1] = 0;

    /* Frame Header */
    tmpbuf[2] = 0x20 | ((0x0700 & frheader.messageId) >> 8);
    tmpbuf[3] = 0x00ff & frheader.messageId;
    tmpbuf[4] = (0xfe & frheader.len) | ((frheader.crc & 0x0400) >> 10);
    tmpbuf[5] = (0x03fc & frheader.crc) >> 2;
    tmpbuf[6] = ((0x0003 & frheader.crc) << 6) | (0x3f & frheader.mux);

    ws_buffer_assure_space(params->buf, sizeof(tmpbuf) + payload_length_valid);
    ws_buffer_append(params->buf, tmpbuf, sizeof(tmpbuf));
    caplen = sizeof(tmpbuf) + payload_length_valid;
    len = sizeof(tmpbuf) + payload_length;

    if (payload_length_valid > 0 && !blf_read_bytes(params, data_start + sizeof(frheader), ws_buffer_end_ptr(params->buf), payload_length_valid, err, err_info)) {
        ws_debug("copying flexray payload failed");
        return FALSE;
    }
    params->buf->first_free += payload_length_valid;

    blf_init_rec(params, flags, object_timestamp, WTAP_ENCAP_FLEXRAY, frheader.channel, UINT16_MAX, caplen, len);
    blf_add_direction_option(params, frheader.dir);

    return TRUE;
}

static gboolean
blf_read_flexraymessage(blf_params_t *params, int *err, gchar **err_info, gint64 block_start, gint64 data_start, gint64 object_length, guint32 flags, guint64 object_timestamp) {
    blf_flexraymessage_t frheader;

    guint8 payload_length;
    guint8 payload_length_valid;
    guint8 tmpbuf[7];
    guint  caplen, len;

    if (object_length < (data_start - block_start) + (int) sizeof(frheader)) {
        *err = WTAP_ERR_BAD_FILE;
        *err_info = ws_strdup_printf("blf: FLEXRAY_MESSAGE: not enough bytes for flexrayheader in object");
        ws_debug("not enough bytes for flexrayheader in object");
        return FALSE;
    }

    if (!blf_read_bytes(params, data_start, &frheader, sizeof(frheader), err, err_info)) {
        ws_debug("not enough bytes for flexrayheader header in file");
        return FALSE;
    }
    fix_endianness_blf_flexraymessage(&frheader);

    payload_length = frheader.length;
    payload_length_valid = payload_length;

    if ((frheader.length & 0x01) == 0x01) {
        ws_debug("reading odd length in FlexRay!?");
    }

    if (payload_length_valid > object_length - (data_start - block_start) - sizeof(frheader)) {
        ws_debug("shortening FlexRay payload because buffer is too short!");
        payload_length_valid = (guint8)(object_length - (data_start - block_start) - sizeof(frheader));
    }

    if (frheader.channel != 0 && frheader.channel != 1) {
        ws_debug("FlexRay supports only two channels.");
    }

    /* Measurement Header */
    if (frheader.channel == 0) {
        tmpbuf[0] = BLF_FLEXRAYDATA_FRAME;
    } else {
        tmpbuf[0] = BLF_FLEXRAYDATA_FRAME | BLF_FLEXRAYDATA_CHANNEL_B;
    }

    /* Error Flags */
    tmpbuf[1] = 0;

    /* Frame Header */
    tmpbuf[2] = ((0x0700 & frheader.frameId) >> 8);
    if ((frheader.frameState & BLF_FLEXRAYMESSAGE_STATE_PPI) == BLF_FLEXRAYMESSAGE_STATE_PPI) {
        tmpbuf[2] |= BLF_DLT_FLEXRAY_PPI;
    }

    if ((frheader.frameState & BLF_FLEXRAYMESSAGE_STATE_SFI) == BLF_FLEXRAYMESSAGE_STATE_SFI) {
        tmpbuf[2] |= BLF_DLT_FLEXRAY_SFI;
    }

    if ((frheader.frameState & BLF_FLEXRAYMESSAGE_STATE_NFI) != BLF_FLEXRAYMESSAGE_STATE_NFI) {
        /* NFI needs to be inversed !? */
        tmpbuf[2] |= BLF_DLT_FLEXRAY_NFI;
    }

    if ((frheader.frameState & BLF_FLEXRAYMESSAGE_STATE_STFI) == BLF_FLEXRAYMESSAGE_STATE_STFI) {
        tmpbuf[2] |= BLF_DLT_FLEXRAY_STFI;
    }

    tmpbuf[3] = 0x00ff & frheader.frameId;
    tmpbuf[4] = (0xfe & frheader.length) | ((frheader.headerCrc & 0x0400) >> 10);
    tmpbuf[5] = (0x03fc & frheader.headerCrc) >> 2;
    tmpbuf[6] = ((0x0003 & frheader.headerCrc) << 6) | (0x3f & frheader.cycle);

    ws_buffer_assure_space(params->buf, sizeof(tmpbuf) + payload_length_valid);
    ws_buffer_append(params->buf, tmpbuf, sizeof(tmpbuf));
    caplen = sizeof(tmpbuf) + payload_length_valid;
    len = sizeof(tmpbuf) + payload_length;

    if (payload_length_valid > 0 && !blf_read_bytes(params, data_start + sizeof(frheader), ws_buffer_end_ptr(params->buf), payload_length_valid, err, err_info)) {
        ws_debug("copying flexray payload failed");
        return FALSE;
    }
    params->buf->first_free += payload_length_valid;

    blf_init_rec(params, flags, object_timestamp, WTAP_ENCAP_FLEXRAY, frheader.channel, UINT16_MAX, caplen, len);
    blf_add_direction_option(params, frheader.dir);

    return TRUE;
}

static gboolean
blf_read_flexrayrcvmessageex(blf_params_t *params, int *err, gchar **err_info, gint64 block_start, gint64 data_start, gint64 object_length, guint32 flags, guint64 object_timestamp, gboolean ext) {
    blf_flexrayrcvmessage_t frheader;

    guint16 payload_length;
    guint16 payload_length_valid;
    guint8  tmpbuf[7];
    gint    frheadersize = sizeof(frheader);
    guint   caplen, len;

    if (ext) {
        frheadersize += 40;
    }

    if ((gint64)object_length < (data_start - block_start) + frheadersize) {
        *err = WTAP_ERR_BAD_FILE;
        *err_info = ws_strdup_printf("blf: %s: not enough bytes for flexrayheader in object",
                                    ext ? "FLEXRAY_RCVMESSAGE_EX" : "FLEXRAY_RCVMESSAGE");
        ws_debug("not enough bytes for flexrayheader in object");
        return FALSE;
    }

    if (!blf_read_bytes(params, data_start, &frheader, sizeof(frheader), err, err_info)) {
        ws_debug("not enough bytes for flexrayheader header in file");
        return FALSE;
    }
    fix_endianness_blf_flexrayrcvmessage(&frheader);

    if (!ext) {
        frheader.dir &= 0xff;
        frheader.cycle &= 0xff;
    }

    payload_length = frheader.payloadLength;
    payload_length_valid = frheader.payloadLengthValid;

    if ((frheader.payloadLength & 0x01) == 0x01) {
        ws_debug("reading odd length in FlexRay!?");
    }

    if (payload_length_valid > object_length - (data_start - block_start) - frheadersize) {
        ws_debug("shortening FlexRay payload because buffer is too short!");
        payload_length_valid = (guint8)(object_length - (data_start - block_start) - frheadersize);
    }

    /* Measurement Header */
    /* TODO: It seems that this format support both channels at the same time!? */
    if (frheader.channelMask == BLF_FLEXRAYRCVMSG_CHANNELMASK_A) {
        tmpbuf[0] = BLF_FLEXRAYDATA_FRAME;
    } else {
        tmpbuf[0] = BLF_FLEXRAYDATA_FRAME | BLF_FLEXRAYDATA_CHANNEL_B;
    }

    /* Error Flags */
    tmpbuf[1] = 0;

    /* Frame Header */
    tmpbuf[2] = ((0x0700 & frheader.frameId) >> 8);
    if ((frheader.data & BLF_FLEXRAYRCVMSG_DATA_FLAG_PAYLOAD_PREAM) == BLF_FLEXRAYRCVMSG_DATA_FLAG_PAYLOAD_PREAM) {
        tmpbuf[2] |= BLF_DLT_FLEXRAY_PPI;
    }

    if ((frheader.data & BLF_FLEXRAYRCVMSG_DATA_FLAG_SYNC) == BLF_FLEXRAYRCVMSG_DATA_FLAG_SYNC) {
        tmpbuf[2] |= BLF_DLT_FLEXRAY_SFI;
    }

    if ((frheader.data & BLF_FLEXRAYRCVMSG_DATA_FLAG_NULL_FRAME) != BLF_FLEXRAYRCVMSG_DATA_FLAG_NULL_FRAME) {
        /* NFI needs to be inversed !? */
        tmpbuf[2] |= BLF_DLT_FLEXRAY_NFI;
    }

    if ((frheader.data & BLF_FLEXRAYRCVMSG_DATA_FLAG_STARTUP) == BLF_FLEXRAYRCVMSG_DATA_FLAG_STARTUP) {
        tmpbuf[2] |= BLF_DLT_FLEXRAY_STFI;
    }

    tmpbuf[3] = 0x00ff & frheader.frameId;
    tmpbuf[4] = (0xfe & frheader.payloadLength) | ((frheader.headerCrc1 & 0x0400) >> 10);
    tmpbuf[5] = (0x03fc & frheader.headerCrc1) >> 2;
    tmpbuf[6] = ((0x0003 & frheader.headerCrc1) << 6) | (0x3f & frheader.cycle);

    ws_buffer_assure_space(params->buf, sizeof(tmpbuf) + payload_length_valid);
    ws_buffer_append(params->buf, tmpbuf, sizeof(tmpbuf));
    caplen = sizeof(tmpbuf) + payload_length_valid;
    len = sizeof(tmpbuf) + payload_length;

    if (payload_length_valid > 0 && !blf_read_bytes(params, data_start + frheadersize, ws_buffer_end_ptr(params->buf), payload_length_valid, err, err_info)) {
        ws_debug("copying flexray payload failed");
        return FALSE;
    }
    params->buf->first_free += payload_length_valid;

    blf_init_rec(params, flags, object_timestamp, WTAP_ENCAP_FLEXRAY, frheader.channelMask, UINT16_MAX, caplen, len);
    blf_add_direction_option(params, frheader.dir);

    return TRUE;
}

static gboolean
blf_read_linmessage(blf_params_t* params, int* err, gchar** err_info, gint64 block_start, gint64 data_start, gint64 object_length, guint32 flags, guint64 object_timestamp) {
    blf_linmessage_t         linmessage;

    guint8  payload_length;
    guint   len;

    if (object_length < (data_start - block_start) + (int)sizeof(linmessage)) {
        *err = WTAP_ERR_BAD_FILE;
        *err_info = ws_strdup_printf("blf: LIN_MESSAGE: not enough bytes for linmessage in object");
        ws_debug("not enough bytes for linmessage in object");
        return FALSE;
    }

    if (!blf_read_bytes(params, data_start, &linmessage, sizeof(linmessage), err, err_info)) {
        ws_debug("not enough bytes for linmessage in file");
        return FALSE;
    }
    fix_endianness_blf_linmessage(&linmessage);

    linmessage.dlc &= 0x0f;
    linmessage.id &= 0x3f;

    payload_length = MIN(linmessage.dlc, 8);

    guint8 tmpbuf[8];
    tmpbuf[0] = 1; /* message format rev = 1 */
    tmpbuf[1] = 0; /* reserved */
    tmpbuf[2] = 0; /* reserved */
    tmpbuf[3] = 0; /* reserved */
    tmpbuf[4] = (linmessage.dlc << 4) | 0; /* dlc (4bit) | type (2bit) | checksum type (2bit) */
    tmpbuf[5] = linmessage.id;  /* parity (2bit) | id (6bit) */
    tmpbuf[6] = (guint8)(linmessage.crc & 0xff); /* checksum */
    tmpbuf[7] = 0; /* errors */

    ws_buffer_assure_space(params->buf, sizeof(tmpbuf) + payload_length);
    ws_buffer_append(params->buf, tmpbuf, sizeof(tmpbuf));
    ws_buffer_append(params->buf, linmessage.data, payload_length);
    len = sizeof(tmpbuf) + payload_length;

    blf_init_rec(params, flags, object_timestamp, WTAP_ENCAP_LIN, linmessage.channel, UINT16_MAX, len, len);
    blf_add_direction_option(params, linmessage.dir);

    return TRUE;
}

static int
blf_read_apptextmessage(blf_params_t *params, int *err, gchar **err_info, gint64 block_start, gint64 data_start, gint64 object_length, guint32 flags, guint64 object_timestamp, gsize metadata_cont) {
    blf_apptext_t            apptextheader;

    if (object_length < (data_start - block_start) + (int)sizeof(apptextheader)) {
        *err = WTAP_ERR_BAD_FILE;
        *err_info = ws_strdup_printf("blf: APP_TEXT: not enough bytes for apptext header in object");
        ws_debug("not enough bytes for apptext header in object");
        return BLF_APPTEXT_FAILED;
    }

    if (!blf_read_bytes(params, data_start, &apptextheader, sizeof(apptextheader), err, err_info)) {
        ws_debug("not enough bytes for apptext header in file");
        return BLF_APPTEXT_FAILED;
    }
    fix_endianness_blf_apptext_header(&apptextheader);

    if (metadata_cont && apptextheader.source != BLF_APPTEXT_METADATA) {
        /* If we're in the middle of a sequence of metadata objects,
         * but we get an AppText object from another source,
         * skip the previously incomplete object and start fresh.
         */
        metadata_cont = 0;
    }

    /* Add an extra byte for a terminating '\0' */
    gchar* text = g_try_malloc((gsize)apptextheader.textLength + 1);

    if (!blf_read_bytes(params, data_start + sizeof(apptextheader), text, apptextheader.textLength, err, err_info)) {
        ws_debug("not enough bytes for apptext text in file");
        g_free(text);
        return BLF_APPTEXT_FAILED;
    }
    text[apptextheader.textLength] = '\0'; /* Here's the '\0' */

    switch (apptextheader.source) {
    case BLF_APPTEXT_CHANNEL:
    {

        /* returns a NULL terminated array of NULL terminates strings */
        gchar** tokens = g_strsplit_set(text, ";", -1);

        if (tokens == NULL || tokens[0] == NULL || tokens[1] == NULL) {
            if (tokens != NULL) {
                g_strfreev(tokens);
            }
            g_free(text);
            return BLF_APPTEXT_CHANNEL;
        }

        guint16 channel = (apptextheader.reservedAppText1 >> 8) & 0xff;
        int pkt_encap;

        switch ((apptextheader.reservedAppText1 >> 16) & 0xff) {
        case BLF_BUSTYPE_CAN:
            pkt_encap = WTAP_ENCAP_SOCKETCAN;
            break;

        case BLF_BUSTYPE_FLEXRAY:
            pkt_encap = WTAP_ENCAP_FLEXRAY;
            break;

        case BLF_BUSTYPE_LIN:
            pkt_encap = WTAP_ENCAP_LIN;
            break;

        case BLF_BUSTYPE_ETHERNET:
            pkt_encap = WTAP_ENCAP_ETHERNET;
            break;

        case BLF_BUSTYPE_WLAN:
            pkt_encap = WTAP_ENCAP_IEEE_802_11;
            break;

        default:
            pkt_encap = 0xffffffff;
        }

        /* we use lookup to create interface, if not existing yet */
        blf_lookup_interface(params, pkt_encap, channel, UINT16_MAX, tokens[1]);

        g_strfreev(tokens);
        g_free(text);
        return BLF_APPTEXT_CHANNEL;
        break;
    }
    case BLF_APPTEXT_METADATA:
        if (metadata_cont) {
            /* Set the buffer pointer to the end of the previous object */
            params->buf->first_free = metadata_cont;
        }
        else {
            /* First object of a sequence of one or more */
            wtap_buffer_append_epdu_string(params->buf, EXP_PDU_TAG_DISSECTOR_NAME, "data-text-lines");
            wtap_buffer_append_epdu_string(params->buf, EXP_PDU_TAG_COL_PROT_TEXT, "BLF App text");
            wtap_buffer_append_epdu_string(params->buf, EXP_PDU_TAG_COL_INFO_TEXT, "Metadata");
            wtap_buffer_append_epdu_end(params->buf);
        }

        ws_buffer_assure_space(params->buf, apptextheader.textLength);
        ws_buffer_append(params->buf, text, apptextheader.textLength);
        g_free(text);

        if ((apptextheader.reservedAppText1 & 0x00ffffff) > apptextheader.textLength) {
            /* Continues in the next object */
            return BLF_APPTEXT_CONT;
        }

        blf_init_rec(params, flags, object_timestamp, WTAP_ENCAP_WIRESHARK_UPPER_PDU, 0, UINT16_MAX, (guint32)ws_buffer_length(params->buf), (guint32)ws_buffer_length(params->buf));
        return BLF_APPTEXT_METADATA;
        break;
    case BLF_APPTEXT_COMMENT:
    case BLF_APPTEXT_ATTACHMENT:
    case BLF_APPTEXT_TRACELINE:
    {
        wtap_buffer_append_epdu_string(params->buf, EXP_PDU_TAG_DISSECTOR_NAME, "data-text-lines");
        wtap_buffer_append_epdu_string(params->buf, EXP_PDU_TAG_COL_PROT_TEXT, "BLF App text");
        switch (apptextheader.source) {
        case BLF_APPTEXT_COMMENT:
            wtap_buffer_append_epdu_string(params->buf, EXP_PDU_TAG_COL_INFO_TEXT, "Comment");
            break;
        case BLF_APPTEXT_ATTACHMENT:
            wtap_buffer_append_epdu_string(params->buf, EXP_PDU_TAG_COL_INFO_TEXT, "Attachment");
            break;
        case BLF_APPTEXT_TRACELINE:
            wtap_buffer_append_epdu_string(params->buf, EXP_PDU_TAG_COL_INFO_TEXT, "Trace line");
            break;
        default:
            break;
        }

        wtap_buffer_append_epdu_end(params->buf);

        gsize text_length = strlen(text);  /* The string can contain '\0' before textLength bytes */
        ws_buffer_assure_space(params->buf, text_length); /* The dissector doesn't need NULL-terminated strings */
        ws_buffer_append(params->buf, text, text_length);

        /* We'll write this as a WS UPPER PDU packet with a text blob */
        blf_init_rec(params, flags, object_timestamp, WTAP_ENCAP_WIRESHARK_UPPER_PDU, 0, UINT16_MAX, (guint32)ws_buffer_length(params->buf), (guint32)ws_buffer_length(params->buf));
        g_free(text);
        return apptextheader.source;
        break;
    }
    default:
        g_free(text);
        return BLF_APPTEXT_CHANNEL; /* Cheat - no block to write */;
        break;
    }
    return BLF_APPTEXT_CHANNEL; /* Cheat - no block to write */
}

static gboolean
blf_read_ethernet_status(blf_params_t* params, int* err, gchar** err_info, gint64 block_start, gint64 data_start, gint64 object_length, guint32 flags, guint64 object_timestamp) {
    blf_ethernet_status_t            ethernet_status_header;
    guint8 tmpbuf[16];

    if (object_length < (data_start - block_start) + (int)sizeof(ethernet_status_header)) {
        *err = WTAP_ERR_BAD_FILE;
        *err_info = ws_strdup_printf("blf: ETHERNET_STATUS: not enough bytes for ethernet status header in object");
        ws_debug("not enough bytes for ethernet status header in object");
        return FALSE;
    }

    if (!blf_read_bytes(params, data_start, &ethernet_status_header, sizeof(ethernet_status_header), err, err_info)) {
        ws_debug("not enough bytes for ethernet_status_header header in file");
        return FALSE;
    }

    fix_endianness_blf_ethernet_status_header(&ethernet_status_header);

    tmpbuf[0] = (ethernet_status_header.channel & 0xff00) >> 8;
    tmpbuf[1] = (ethernet_status_header.channel & 0x00ff);
    tmpbuf[2] = (ethernet_status_header.flags & 0xff00) >> 8;
    tmpbuf[3] = (ethernet_status_header.flags & 0x00ff);
    tmpbuf[4] = (ethernet_status_header.linkStatus);
    tmpbuf[5] = (ethernet_status_header.ethernetPhy);
    tmpbuf[6] = (ethernet_status_header.duplex);
    tmpbuf[7] = (ethernet_status_header.mdi);
    tmpbuf[8] = (ethernet_status_header.connector);
    tmpbuf[9] = (ethernet_status_header.clockMode);
    tmpbuf[10] = (ethernet_status_header.pairs);
    tmpbuf[11] = (ethernet_status_header.hardwareChannel);
    tmpbuf[12] = (ethernet_status_header.bitrate & 0xff000000) >> 24;
    tmpbuf[13] = (ethernet_status_header.bitrate & 0x00ff0000) >> 16;
    tmpbuf[14] = (ethernet_status_header.bitrate & 0x0000ff00) >> 8;
    tmpbuf[15] = (ethernet_status_header.bitrate & 0x000000ff);

    wtap_buffer_append_epdu_string(params->buf, EXP_PDU_TAG_DISSECTOR_NAME, "blf-ethernetstatus-obj");
    wtap_buffer_append_epdu_end(params->buf);

    ws_buffer_assure_space(params->buf, sizeof(ethernet_status_header));
    ws_buffer_append(params->buf, tmpbuf, (gsize)16);

    /* We'll write this as a WS UPPER PDU packet with a data blob */
    /* This will create an interface with the "name" of the matching
     * WTAP_ENCAP_ETHERNET interface with the same channel and hardware
     * channel prefixed with "STATUS" and with a different interface ID,
     * because IDBs in pcapng can only have one linktype.
     * The other option would be to write everything as UPPER_PDU, including
     * the Ethernet data (with one of the "eth_" dissectors.)
     */
    char* iface_name = ws_strdup_printf("STATUS-ETH-%u-%u", ethernet_status_header.channel, ethernet_status_header.hardwareChannel);
    blf_lookup_interface(params, WTAP_ENCAP_WIRESHARK_UPPER_PDU, ethernet_status_header.channel, ethernet_status_header.hardwareChannel, iface_name);
    g_free(iface_name);
    blf_init_rec(params, flags, object_timestamp, WTAP_ENCAP_WIRESHARK_UPPER_PDU, ethernet_status_header.channel, ethernet_status_header.hardwareChannel, (guint32)ws_buffer_length(params->buf), (guint32)ws_buffer_length(params->buf));

    if ((ethernet_status_header.flags & BLF_ETH_STATUS_HARDWARECHANNEL) == BLF_ETH_STATUS_HARDWARECHANNEL) {
        /* If HW channel valid */
        wtap_block_add_uint32_option(params->rec->block, OPT_PKT_QUEUE, ethernet_status_header.hardwareChannel);
    }

    return TRUE;
}

static gboolean
blf_read_block(blf_params_t *params, gint64 start_pos, int *err, gchar **err_info) {
    blf_blockheader_t        header;
    blf_logobjectheader_t    logheader;
    blf_logobjectheader2_t   logheader2;
    blf_logobjectheader3_t   logheader3;
    guint32                  flags;
    guint64                  object_timestamp;
    gint64                   last_metadata_start = 0;
    gsize                    metadata_cont = 0;

    while (1) {
        /* Find Object */

        /* Resetting buffer */
        params->buf->first_free = params->buf->start;

        while (1) {
            if (!blf_read_bytes_or_eof(params, start_pos, &header, sizeof header, err, err_info)) {
                ws_debug("not enough bytes for block header or unsupported file");
                if (*err == WTAP_ERR_SHORT_READ) {
                    /* we have found the end that is not a short read therefore. */
                    *err = 0;
                    g_free(*err_info);
                }
                return FALSE;
            }

            fix_endianness_blf_blockheader(&header);

            if (memcmp(header.magic, blf_obj_magic, sizeof(blf_obj_magic))) {
                ws_debug("object magic is not LOBJ (pos: 0x%" PRIx64 ")", start_pos);
            } else {
                break;
            }

            /* we are moving back and try again but 1 byte later */
            /* TODO: better understand how this paddings works... */
            start_pos++;
        }
        params->blf_data->start_of_last_obj = start_pos;

        switch (header.header_type) {
        case BLF_HEADER_TYPE_DEFAULT:
            if (!blf_read_log_object_header(params, err, err_info, start_pos + sizeof(blf_blockheader_t), start_pos + header.header_length, &logheader)) {
                return FALSE;
            }
            flags = logheader.flags;
            object_timestamp = logheader.object_timestamp;
            break;

        case BLF_HEADER_TYPE_2:
            if (!blf_read_log_object_header2(params, err, err_info, start_pos + sizeof(blf_blockheader_t), start_pos + header.header_length, &logheader2)) {
                return FALSE;
            }
            flags = logheader2.flags;
            object_timestamp = logheader2.object_timestamp;
            break;

        case BLF_HEADER_TYPE_3:
            if (!blf_read_log_object_header3(params, err, err_info, start_pos + sizeof(blf_blockheader_t), start_pos + header.header_length, &logheader3)) {
                return FALSE;
            }
            flags = logheader3.flags;
            object_timestamp = logheader3.object_timestamp;
            break;

        default:
            *err = WTAP_ERR_UNSUPPORTED;
            *err_info = ws_strdup_printf("blf: unknown header type %u", header.header_type);
            ws_debug("unknown header type");
            return FALSE;
        }

        /* already making sure that we start after this object next time. */
        params->blf_data->current_real_seek_pos = start_pos + MAX(MAX(16, header.object_length), header.header_length);

        if (metadata_cont && header.object_type != BLF_OBJTYPE_APP_TEXT) {
            /* If we're in the middle of a sequence of AppText metadata objects,
             * but we get an AppText object from another source,
             * skip the previous incomplete packet and start fresh.
             */
            metadata_cont = 0;
            last_metadata_start = 0;
        }

        switch (header.object_type) {
        case BLF_OBJTYPE_LOG_CONTAINER:
            *err = WTAP_ERR_UNSUPPORTED;
            *err_info = ws_strdup_printf("blf: log container in log container not supported");
            ws_debug("log container in log container not supported");
            return FALSE;
            break;

        case BLF_OBJTYPE_ETHERNET_FRAME:
            return blf_read_ethernetframe(params, err, err_info, start_pos, start_pos + header.header_length, header.object_length, flags, object_timestamp);
            break;

        case BLF_OBJTYPE_ETHERNET_FRAME_EX:
            return blf_read_ethernetframe_ext(params, err, err_info, start_pos, start_pos + header.header_length, header.object_length, flags, object_timestamp);
            break;

        case BLF_OBJTYPE_WLAN_FRAME:
            return blf_read_wlanframe(params, err, err_info, start_pos, start_pos + header.header_length, header.object_length, flags, object_timestamp);
            break;

        case BLF_OBJTYPE_CAN_MESSAGE:
            return blf_read_canmessage(params, err, err_info, start_pos, start_pos + header.header_length, header.object_length, flags, object_timestamp, FALSE);
            break;

        case BLF_OBJTYPE_CAN_ERROR:
            return blf_read_canerror(params, err, err_info, start_pos, start_pos + header.header_length, header.object_length, flags, object_timestamp);
            break;

        case BLF_OBJTYPE_CAN_MESSAGE2:
            return blf_read_canmessage(params, err, err_info, start_pos, start_pos + header.header_length, header.object_length, flags, object_timestamp, TRUE);
            break;

        case BLF_OBJTYPE_CAN_ERROR_EXT:
            return blf_read_canerrorext(params, err, err_info, start_pos, start_pos + header.header_length, header.object_length, flags, object_timestamp);
            break;

        case BLF_OBJTYPE_CAN_FD_MESSAGE:
            return blf_read_canfdmessage(params, err, err_info, start_pos, start_pos + header.header_length, header.object_length, flags, object_timestamp);
            break;

        case BLF_OBJTYPE_CAN_FD_MESSAGE_64:
            return blf_read_canfdmessage64(params, err, err_info, start_pos, start_pos + header.header_length, header.object_length, flags, object_timestamp);
            break;

        case BLF_OBJTYPE_CAN_FD_ERROR_64:
            return blf_read_canfderror64(params, err, err_info, start_pos, start_pos + header.header_length, header.object_length, flags, object_timestamp);
            break;

        case BLF_OBJTYPE_FLEXRAY_DATA:
            return blf_read_flexraydata(params, err, err_info, start_pos, start_pos + header.header_length, header.object_length, flags, object_timestamp);
            break;

        case BLF_OBJTYPE_FLEXRAY_MESSAGE:
            return blf_read_flexraymessage(params, err, err_info, start_pos, start_pos + header.header_length, header.object_length, flags, object_timestamp);
            break;

        case BLF_OBJTYPE_FLEXRAY_RCVMESSAGE:
            return blf_read_flexrayrcvmessageex(params, err, err_info, start_pos, start_pos + header.header_length, header.object_length, flags, object_timestamp, FALSE);
            break;

        case BLF_OBJTYPE_FLEXRAY_RCVMESSAGE_EX:
            return blf_read_flexrayrcvmessageex(params, err, err_info, start_pos, start_pos + header.header_length, header.object_length, flags, object_timestamp, TRUE);
            break;

        case BLF_OBJTYPE_LIN_MESSAGE:
            return blf_read_linmessage(params, err, err_info, start_pos, start_pos + header.header_length, header.object_length, flags, object_timestamp);
            break;

        case BLF_OBJTYPE_APP_TEXT:
        {
            int result = blf_read_apptextmessage(params, err, err_info, start_pos, start_pos + header.header_length, header.object_length, flags, object_timestamp, metadata_cont);
            if (result == BLF_APPTEXT_CONT) {
                if (!metadata_cont) {
                    /* First object of a sequence, save its start position */
                    last_metadata_start = start_pos;
                }
                /* Save a pointer to the end of the buffer */
                metadata_cont = params->buf->first_free;
            }
            else {
                if (result == BLF_APPTEXT_METADATA && metadata_cont) {
                    /* Last object of a sequence, restore the start position of the first object */
                    params->blf_data->start_of_last_obj = last_metadata_start;
                }
                /* Reset everything and start fresh */
                last_metadata_start = 0;
                metadata_cont = 0;
            }
            switch (result) {
                case BLF_APPTEXT_FAILED:
                    return FALSE;
                case BLF_APPTEXT_COMMENT:
                case BLF_APPTEXT_METADATA:
                case BLF_APPTEXT_ATTACHMENT:
                case BLF_APPTEXT_TRACELINE:
                    return TRUE;
                case BLF_APPTEXT_CHANNEL:
                case BLF_APPTEXT_CONT:
                default:
                    /* we do not return since there is no packet to show here */
                    start_pos += MAX(MAX(16, header.object_length), header.header_length);
            }
        }
            break;

        case BLF_OBJTYPE_ETHERNET_STATUS:
            return blf_read_ethernet_status(params, err, err_info, start_pos, start_pos + header.header_length, header.object_length, flags, object_timestamp);
            break;
        default:
            ws_debug("unknown object type 0x%04x", header.object_type);
            start_pos += MAX(MAX(16, header.object_length), header.header_length);
        }
    }
    return TRUE;
}

static gboolean blf_read(wtap *wth, wtap_rec *rec, Buffer *buf, int *err, gchar **err_info, gint64 *data_offset) {
    blf_params_t blf_tmp;

    blf_tmp.wth = wth;
    blf_tmp.fh  = wth->fh;
    blf_tmp.rec = rec;
    blf_tmp.buf = buf;
    blf_tmp.blf_data = (blf_t *)wth->priv;

    if (!blf_read_block(&blf_tmp, blf_tmp.blf_data->current_real_seek_pos, err, err_info)) {
        return FALSE;
    }
    *data_offset = blf_tmp.blf_data->start_of_last_obj;

    return TRUE;
}

static gboolean blf_seek_read(wtap *wth, gint64 seek_off, wtap_rec *rec, Buffer *buf, int *err, gchar **err_info) {
    blf_params_t blf_tmp;

    blf_tmp.wth = wth;
    blf_tmp.fh  = wth->random_fh;
    blf_tmp.rec = rec;
    blf_tmp.buf = buf;
    blf_tmp.blf_data = (blf_t *)wth->priv;

    if (!blf_read_block(&blf_tmp, seek_off, err, err_info)) {
        ws_debug("couldn't read packet block (err=%d).", *err);
        return FALSE;
    }

    return TRUE;
}

static void blf_close(wtap *wth) {
    blf_t *blf = (blf_t *)wth->priv;

    if (blf != NULL && blf->log_containers != NULL) {
        for (guint i = 0; i < blf->log_containers->len; i++) {
            blf_log_container_t *log_container = &g_array_index(blf->log_containers, blf_log_container_t, i);
            if (log_container->real_data != NULL) {
                g_free(log_container->real_data);
            }
        }
        g_array_free(blf->log_containers, TRUE);
        blf->log_containers = NULL;
    }

    if (blf != NULL && blf->channel_to_iface_ht != NULL) {
        g_hash_table_destroy(blf->channel_to_iface_ht);
        blf->channel_to_iface_ht = NULL;
    }

    /* TODO: do we need to reverse the wtap_add_idb? how? */

    return;
}

wtap_open_return_val
blf_open(wtap *wth, int *err, gchar **err_info) {
    blf_fileheader_t  header;
    blf_t            *blf;
    blf_params_t      params;

    ws_debug("opening file");

    if (!wtap_read_bytes_or_eof(wth->fh, &header, sizeof header, err, err_info)) {

        ws_debug("wtap_read_bytes_or_eof() failed, err = %d.", *err);
        if (*err == 0 || *err == WTAP_ERR_SHORT_READ) {
            /*
             * Short read or EOF.
             *
             * We're reading this as part of an open, so
             * the file is too short to be a blf file.
             */
            *err = 0;
            g_free(*err_info);
            *err_info = NULL;
            return WTAP_OPEN_NOT_MINE;
        }
        return WTAP_OPEN_ERROR;
    }

    fix_endianness_blf_fileheader(&header);

    if (memcmp(header.magic, blf_magic, sizeof(blf_magic))) {
        return WTAP_OPEN_NOT_MINE;
    }

    /* This seems to be an BLF! */
    /* skip unknown part of header */
    file_seek(wth->fh, header.header_length, SEEK_SET, err);

    struct tm timestamp;
    timestamp.tm_year = (header.start_date.year > 1970) ? header.start_date.year - 1900 : 70;
    timestamp.tm_mon  = header.start_date.month -1;
    timestamp.tm_mday = header.start_date.day;
    timestamp.tm_hour = header.start_date.hour;
    timestamp.tm_min  = header.start_date.mins;
    timestamp.tm_sec  = header.start_date.sec;
    timestamp.tm_isdst = -1;

    /* Prepare our private context. */
    blf = g_new(blf_t, 1);
    blf->log_containers = NULL;
    blf->current_log_container = 0;
    blf->current_real_seek_pos = 0;
    blf->start_offset_ns = 1000 * 1000 * 1000 * (guint64)mktime(&timestamp);
    blf->start_offset_ns += 1000 * 1000 * header.start_date.ms;

    blf->channel_to_iface_ht = g_hash_table_new_full(g_int64_hash, g_int64_equal, &blf_free_key, &blf_free_channel_to_iface_entry);
    blf->next_interface_id = 0;

    /* embed in params */
    params.blf_data = blf;
    params.buf = NULL;
    params.fh = wth->fh;
    params.rec = NULL;
    params.wth = wth;
    params.blf_data->current_real_seek_pos = 0;

    /* lets check out the layout of all log containers */
    blf_scan_file_for_logcontainers(&params);

    wth->priv = (void *)blf;
    wth->file_encap = WTAP_ENCAP_NONE;
    wth->snapshot_length = 0;
    wth->file_tsprec = WTAP_TSPREC_UNKNOWN;
    wth->subtype_read = blf_read;
    wth->subtype_seek_read = blf_seek_read;
    wth->subtype_close = blf_close;
    wth->file_type_subtype = blf_file_type_subtype;

    return WTAP_OPEN_MINE;
}

/* Options for interface blocks. */
static const struct supported_option_type interface_block_options_supported[] = {
    /* No comments, just an interface name. */
    { OPT_IDB_NAME, ONE_OPTION_SUPPORTED }
};

static const struct supported_block_type blf_blocks_supported[] = {
    { WTAP_BLOCK_PACKET, MULTIPLE_BLOCKS_SUPPORTED, NO_OPTIONS_SUPPORTED },
    { WTAP_BLOCK_IF_ID_AND_INFO, MULTIPLE_BLOCKS_SUPPORTED, OPTION_TYPES_SUPPORTED(interface_block_options_supported) },
};

static const struct file_type_subtype_info blf_info = {
        "Vector Informatik Binary Logging Format (BLF) logfile", "blf", "blf", NULL,
        FALSE, BLOCKS_SUPPORTED(blf_blocks_supported),
        NULL, NULL, NULL
};

void register_blf(void)
{
    blf_file_type_subtype = wtap_register_file_type_subtype(&blf_info);

    /*
     * Register name for backwards compatibility with the
     * wtap_filetypes table in Lua.
     */
    wtap_register_backwards_compatibility_lua_name("BLF", blf_file_type_subtype);
}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
