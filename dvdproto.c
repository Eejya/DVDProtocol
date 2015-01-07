/*
 * Copyright (c) 2009 Erik Van Grunderbeeck <erik <at> arawix.com>
 * Copyright (c) 2012 Stefano Sabatini
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <dvdnav/dvdnav.h>
#include <dvdnav/dvd_types.h>
#include "libavutil/avstring.h"
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavformat/url.h"

#define DVD_PROTO_PREFIX "dvd:"

#define MAX_ATTACH_BUTTONS             36
#define MAX_ATTACH_AUDIO_LANGUAGES      8
#define MAX_ATTACH_SUB_LANGUAGES       32

typedef enum {
    DVDEffectUnknown,
    DVDEffectButtons,
    DVDEffectButtonsHighlight,
    DVDEffectWait,
    DVDEffectCLUT,
    DVDEffectClearQueue,
    DVDEffectResetIFO,
    DVDEffectClearNOP,
} DVDEffectType;

/* contains the map the dvd, buffer and event data */
typedef struct {
    dvdnav_t   *nav_data;
    uint8_t     cache_buf[DVD_VIDEO_LB_LEN];
    uint8_t    *read_buf;
    uint8_t    *next_buf;
    int         read_buf_left;
    char *language;
} DVDContext;

typedef struct {
    uint16_t x, y, w, h;
} DVDAttachmentButton;

typedef struct {
    int64_t pts;     ///< pts of this context
    uint16_t type;
    uint16_t wait;

    // when buttons
    uint16_t highlight_index;
    uint16_t button_count;
    DVDAttachmentButton buttons[MAX_ATTACH_BUTTONS];

    // when color-palette
    uint32_t   rgba_palette[16];

    // expected size of the video pictures (allows for skip of scan-stream)
    uint16_t video_width, video_height;

    // aspect ratio (0 = 4:3 , 3 = 16:9)
    uint8_t aspect_ratio : 4;
    // video format (0 = ntsc, 1 = pal)
    uint8_t video_format : 4;

    uint8_t current_vts;
    uint8_t nb_audio_languages;
    uint8_t nb_subtitle_languages;

    uint16_t audio_languages   [MAX_ATTACH_AUDIO_LANGUAGES];
    uint16_t audio_flags       [MAX_ATTACH_AUDIO_LANGUAGES];
    uint8_t  audio_channels    [MAX_ATTACH_AUDIO_LANGUAGES];
    uint8_t  audio_mode        [MAX_ATTACH_AUDIO_LANGUAGES];
    uint16_t subtitle_languages[MAX_ATTACH_SUB_LANGUAGES];
    uint16_t subtitle_flags    [MAX_ATTACH_SUB_LANGUAGES];

    // when angle change
    uint8_t current_angle;
    uint8_t max_angle;

    // size of current title in pts ticks. divide by 90000 to get time in seconds
    uint64_t duration;

    // flags that describe actions allowed for current chapter
    uint32_t flags;
} DVDParseContext;

/* use libdvdnav's read ahead cache? */
#define DVD_READ_CACHE 1

/* which is the default language for menus/audio/subpictures? */
#define DVD_LANGUAGE "en"

static uint16_t bswap_16(uint16_t bswap)
{
    uint16_t a = bswap & 0xff;
    uint16_t b = (bswap >> 8) & 0xff;
    return ((a << 8) | b);
}

static void dvd_protocol_send(URLContext *h, DVDParseContext *pctx)
{
    /* If there is a user-supplied mutex locking routine, call it. */
    /* if (ff_dvd_lockmgr_cb) { */
    /*     if ((*ff_dvd_lockmgr_cb)(&codec_dvd_mutex, AV_LOCK_OBTAIN)) */
    /*         return; */
    /* } */

    /* if (h->ff_protocol_cb) { */
    /*     (h->ff_protocol_cb)(pctx, h->ff_cb_userdata); */
    /* } */

    /* /\* If there is a user-supplied mutex locking routine, call it. *\/ */
    /* if (ff_dvd_lockmgr_cb) { */
    /*     if ((*ff_dvd_lockmgr_cb)(&codec_dvd_mutex, AV_LOCK_RELEASE)) */
    /*         return; */
    /* } */
}


static int dvd_protocol_build_packet_reset(URLContext *h, DVDContext *dvd,
                                           dvdnav_vts_change_event_t *event)
{
    int i, iCount;
    int32_t ititle, ipart;
    uint64_t *times;
    uint32_t i_width, i_height;
    dvdnav_status_t eStatus;
    audio_attr_t iAudioAttr;
    subp_attr_t iSubAttr;
    av_unused video_attr_t iVidAttr;
    DVDParseContext *pctx;
    int32_t current_angle, max_angle;

    // check the type
    if (event->new_domain == DVD_READ_TITLE_VOBS) {
        /* in general, this signals that we should re-read the
         * language and sub-title information from the dvd
         * this is the best method I could find, since not all
         *  dvd's send a ifo change domain (usually just send at
         * beginning)
         */

        pctx = (DVDParseContext *)av_mallocz(sizeof(DVDParseContext));
        if (!pctx)
            return AVERROR(ENOMEM);

        pctx->type = DVDEffectResetIFO;
        pctx->pts = dvdnav_get_current_time(dvd->nav_data);

        // set width and height
        /* if (dvdnav_get_video_attributes(dvd->nav_data, &iVidAttr) == DVDNAV_STATUS_OK) { */
        /*     pctx->video_format = iVidAttr.video_format; */
        /*     pctx->aspect_ratio = iVidAttr.display_aspect_ratio; */
        /*     pctx->video_width = 640; */
        /*     pctx->video_height = 480; */
        /*     if (iVidAttr.video_format) { */
        /*         pctx->video_height = 576; */
        /*         switch (iVidAttr.picture_size) { */
        /*         case 0: pctx->video_width = 720; break; */
        /*         case 1: pctx->video_width = 704; break; */
        /*         case 2: pctx->video_width = 352; break; */
        /*         case 3: */
        /*             pctx->video_width = 352; */
        /*             pctx->video_height /= 2; */
        /*             break; */
        /*         } */
        /*     } */
        /* } */

		if( dvdnav_get_video_resolution(dvd->nav_data,&i_width, &i_height ) )
      	i_width = i_height = 0;
		switch( dvdnav_get_video_aspect(dvd->nav_data) )
		{
		case 0:
			pctx->video_height = 4 * i_height;
			pctx->video_width = 3 * i_width;
			break;
		case 3:
			pctx->video_height = 16 * i_height;
			pctx->video_width = 9 * i_width;
			break;
		default:
			pctx->video_height = 0;
			pctx->video_width = 0;
			break;
		}
        // store vts index
        pctx->current_vts = (uint8_t)event->new_vtsN;

        // get all audio languages, theres always max 8
        eStatus = DVDNAV_STATUS_OK;
        memset(&pctx->audio_languages, 0, sizeof(uint16_t)*MAX_ATTACH_AUDIO_LANGUAGES);
        memset(&pctx->audio_flags, 0,     sizeof(uint16_t)*MAX_ATTACH_AUDIO_LANGUAGES);
        for (i = 0; i < MAX_ATTACH_AUDIO_LANGUAGES && eStatus == DVDNAV_STATUS_OK; i++) {
            eStatus = dvdnav_get_audio_attr(dvd->nav_data, i, &iAudioAttr);

            pctx->audio_languages[i] = bswap_16(iAudioAttr.lang_code);
            pctx->audio_flags    [i] = iAudioAttr.code_extension;
            pctx->audio_channels [i] = iAudioAttr.channels;
            pctx->audio_mode     [i] = iAudioAttr.application_mode;

            // stop when language empty
            if (iAudioAttr.lang_code == 0x0000)
                break;
        }
        pctx->nb_audio_languages = i;

        //get information for subtitle languages
        iCount = 0;
        eStatus = DVDNAV_STATUS_OK;
        memset(&pctx->subtitle_languages, 0, sizeof(uint16_t)*MAX_ATTACH_SUB_LANGUAGES);
        memset(&pctx->subtitle_languages, 0, sizeof(uint16_t)*MAX_ATTACH_SUB_LANGUAGES);
        int spu = dvdnav_get_active_spu_stream(dvd->nav_data);
        av_log(h,0,"Active SPU stream %d\n",spu);
        for (i = 0; i < MAX_ATTACH_SUB_LANGUAGES && eStatus == DVDNAV_STATUS_OK; i++) {
            // get the attributes, make sure we dont get the same stuff twice (seen that happen)
            eStatus = dvdnav_get_spu_attr(dvd->nav_data, i, &iSubAttr);

            pctx->subtitle_languages[iCount] = bswap_16(iSubAttr.lang_code);
            /* from the documentation at http://dvd.sourceforge.net/dvdinfo/sprm.html
               1=normal, 2=large, 3=children, 5=normal captions, 6=large captions, 7=childrens captions,
               9=forced, 13=director comments, 14=large director comments, 15=director comments for children */
            pctx->subtitle_flags[i] = iSubAttr.code_extension;

            // stop when language empty
            if (iSubAttr.lang_code == 0x0000)
                break;

            ++iCount;
        }
        pctx->nb_subtitle_languages = iCount;

        // get time of vobs. might be a better way?
        ititle = 0;
        ipart = 0;
        times = NULL;
        if (dvdnav_current_title_info(dvd->nav_data, &ititle, &ipart) == DVDNAV_STATUS_OK) {
            dvdnav_describe_title_chapters(dvd->nav_data, ititle, &times, &pctx->duration);
            if (times) {
                /* note that I can't use av_free here (since the memory isnt libav's) */
                av_free(times);
            }
        }

        // get angle info
        current_angle = max_angle = 0;
        if (dvdnav_get_angle_info(dvd->nav_data, &current_angle, &max_angle) == DVDNAV_STATUS_OK) {
            pctx->current_angle = (uint8_t)current_angle;
            pctx->max_angle     = (uint8_t)max_angle;
        }

        dvd_protocol_send(h, pctx);
        return 1;
    }

    return 0;
}

/**
 * Build a navigation packet signaling new button selection data.
 */
static int dvd_protocol_build_packet_nav(URLContext *h, DVDContext *dvd)
{
    DVDParseContext *pctx;
    pci_t *pci = dvdnav_get_current_nav_pci(dvd->nav_data);
    int i;
    int iButtonCount = pci->hli.hl_gi.btn_ns;
    if (iButtonCount == 0)
        return 0;

    pctx = (DVDParseContext *)av_mallocz(sizeof(DVDParseContext));
    if (!pctx)
        return AVERROR(ENOMEM);

    pctx->type = DVDEffectButtons;
    pctx->pts = dvdnav_get_current_time(dvd->nav_data);
    pctx->button_count = (uint16_t)iButtonCount;
    pctx->button_count = FFMIN(pctx->button_count, MAX_ATTACH_BUTTONS);

    for (i = 0; i < iButtonCount; i++) {
        btni_t *btni = &(pci->hli.btnit[i]);
        pctx->buttons[i].x = btni->x_start;
        pctx->buttons[i].y = btni->y_start;
        pctx->buttons[i].w = btni->x_end - btni->x_start;
        pctx->buttons[i].h = btni->y_end - btni->y_start;
    }

    dvd_protocol_send(h, pctx);
    return 1;
}

/**
 * Build a navigation packet signaling new button highlight data.
 */
static int dvd_protocol_build_packet_highlight(URLContext *h, DVDContext *dvd,
                                               uint32_t index)
{
    DVDParseContext *pctx = (DVDParseContext *)av_mallocz(sizeof(DVDParseContext));
    if (!pctx)
        return AVERROR(ENOMEM);

    pctx->type = DVDEffectButtonsHighlight;
    pctx->pts = dvdnav_get_current_time(dvd->nav_data);
    pctx->highlight_index = index > 0 ? index-1 : 0;

    pctx->buttons[0] = (DVDAttachmentButton){0};
    dvd_protocol_send(h, pctx);

    return 1;
}

/**
 * Build a navigation packet signaling a new color table.
 */
static int dvd_protocol_build_packet_clut(URLContext *h, DVDContext *dvd, uint32_t *data_color)
{
    DVDParseContext *pctx = (DVDParseContext *)av_mallocz(sizeof(DVDParseContext));
    if (!pctx)
        return AVERROR(ENOMEM);

    pctx->type = DVDEffectCLUT;
    pctx->pts = dvdnav_get_current_time(dvd->nav_data);
    memcpy(pctx->rgba_palette, data_color, 16 * sizeof(uint32_t));
    dvd_protocol_send(h, pctx);

    return 1;
}

/**
 * Build a navigation packet signaling a wait (e.g. on still).
 */
static int dvd_protocol_build_packet_wait(URLContext *h, DVDContext *dvd, uint32_t time)
{
    DVDParseContext *pctx = (DVDParseContext *)av_mallocz(sizeof(DVDParseContext));
    if (!pctx)
        return AVERROR(ENOMEM);

    pctx->type = DVDEffectWait;
    pctx->pts = dvdnav_get_current_time(dvd->nav_data);
    pctx->wait = time;
    dvd_protocol_send(h, pctx);
    return 1;
}

/**
 * Take part of the buffer and send it back.
 */
static int dvd_protocol_read_build(URLContext *h, DVDContext *dvd, unsigned char *buf_out, int size)
{
		   
    int min_len_read = FFMIN(dvd->read_buf_left, size);
    
    
    memcpy(buf_out, dvd->next_buf, min_len_read);
    dvd->read_buf_left -= min_len_read;
    dvd->next_buf      += min_len_read;
    return min_len_read;
}

static int dvd_open(URLContext *h, const char *filename, int flags)
{
    DVDContext *dvd = h->priv_data;
    const char *diskname = filename;
    int i = 1;
	int32_t titles;
    uint64_t *times,duration,max_duration,max_duration_title = 1;

    av_strstart(filename, DVD_PROTO_PREFIX, &diskname);

    dvd->language = av_strdup(DVD_LANGUAGE);
    dvd->read_buf = dvd->cache_buf;

    if (dvdnav_open(&dvd->nav_data, diskname) != DVDNAV_STATUS_OK) {
        av_freep(&dvd);
        return AVERROR(EIO);
    }

    /* set read ahead cache usage */
    if (dvdnav_set_readahead_flag(dvd->nav_data, DVD_READ_CACHE) != DVDNAV_STATUS_OK) {
        dvdnav_close(dvd->nav_data);
        av_freep(&dvd);
        return AVERROR(EACCES);
    }

    /* set the language */
    if ((dvdnav_menu_language_select (dvd->nav_data, dvd->language) != DVDNAV_STATUS_OK)  ||
        (dvdnav_audio_language_select(dvd->nav_data, dvd->language) != DVDNAV_STATUS_OK)  ||
        (dvdnav_spu_language_select  (dvd->nav_data, dvd->language) != DVDNAV_STATUS_OK)) {
        av_log(h, AV_LOG_ERROR, "Error selecting language\n");
        dvdnav_close(dvd->nav_data);
        av_freep(&dvd);
        return AVERROR(EACCES);
    }

    /* set the PGC positioning flag to have position information relatively to the
     * current chapter (seek will seek in the chapter) */
    if (dvdnav_set_PGC_positioning_flag(dvd->nav_data, 0) != DVDNAV_STATUS_OK) {
        av_log(h, AV_LOG_ERROR, "Error setting PGC positioning flags\n");
        dvdnav_close(dvd->nav_data);
        av_freep(&dvd);
        return AVERROR(EACCES);
    }

    h->priv_data = dvd;

	if(dvdnav_get_number_of_titles(dvd->nav_data , &titles) != DVDNAV_STATUS_OK)
		av_log( h, 0,"Error Getting No. of Titles");

	/*Selecting the title with the with the longest duration and playing it*/
	while(i <= titles)
	{
		dvdnav_describe_title_chapters(dvd->nav_data , i , &times, &duration);
		av_log( h, 0,"The duration of the chapter no. %d is %d\n", i, duration);
		if(duration > max_duration){
			max_duration = duration;
			max_duration_title = i;
		}	
		i++;
	}	
	av_log( h, 0,"Title with maximum duration is %d\n",max_duration_title);
	dvdnav_title_play(dvd->nav_data , max_duration_title);

    
    return 0;
}

static int dvd_close(URLContext *h)
{
    DVDContext *dvd = (DVDContext *)h->priv_data;
    av_freep(&dvd->language);
    dvdnav_close(dvd->nav_data);
    return 0;
}

static int dvd_read(URLContext *h, unsigned char *buf_out, int size)
{
    DVDContext *dvd = (DVDContext *)h->priv_data;
    int i=0,res, event, read_len,finished=0;
	    
    if (!dvd)
        return AVERROR(EFAULT);

while(!finished)
{
	#ifdef DVD_READ_CACHE
	    res = dvdnav_get_next_cache_block(dvd->nav_data, &dvd->read_buf , &event, &read_len);
	#else
	    res = dvdnav_get_next_block(dvd->nav_data, &dvd->read_buf, &event, &read_len);
	#endif
    if (res == DVDNAV_STATUS_ERR)
        return AVERROR(EIO);
	switch(event)
	{	
    case DVDNAV_BLOCK_OK:
		memcpy(buf_out,dvd->read_buf,read_len);
		av_log(h,0,"Copied the buffer value to buf_out\n");
		return read_len;
		break;

    case DVDNAV_NAV_PACKET:
		av_log(h,0,"Nav Packet\n");
		break;

     case DVDNAV_HIGHLIGHT:
        av_log(h,0,"Highlight Packet\n");
        break;

    case DVDNAV_SPU_CLUT_CHANGE:
        av_log(h,0,"SPU CLUT Packet\n");
        break;

    case DVDNAV_STILL_FRAME:
        av_log(h,0,"Still Frame\n");
        break;

    case DVDNAV_WAIT:
        av_log(h,0,"Wait Packet\n");
        break;

     case DVDNAV_VTS_CHANGE:
         av_log(h,0,"VTS Change Packet\n");
         break; 

    case DVDNAV_CELL_CHANGE:
        av_log(h,0,"Cell Change Packet\n");
        break;

    case DVDNAV_HOP_CHANNEL:
    	av_log(h,0,"Hop Channel Packet\n");
        break;

    case DVDNAV_SPU_STREAM_CHANGE:
        av_log(h,0,"SPU Stream Change\n");
        break;

    case DVDNAV_AUDIO_STREAM_CHANGE:
        av_log(h,0,"Audio Stream Change\n");
        break;

    case DVDNAV_NOP:
    	av_log(h,0,"NOP Nothing to do");
        break;

    case DVDNAV_STOP:
    default:
    finished=1;
    /* Playback should end here. */
    break;
    }

}
// we stop
    if (dvd->next_buf) {
	#ifdef DVD_READ_CACHE
	    dvdnav_free_cache_block(dvd->nav_data, dvd->read_buf);
	#endif
        dvd->next_buf = NULL;
    }

    return 0;
}

/**
 * Seek in the DVD. Only used for chapters seek, not seeking in the
 * VOB (PTS seek) itself.
 */
/*
static int64_t dvd_read_seek(URLContext *h, int stream_index, int64_t pos, int flags)
{
    av_unused dvdnav_status_t res;
    av_unused int64_t loop;
    DVDContext *dvd = (DVDContext *)h->priv_data;

    if (!dvd)
        return AVERROR(EFAULT);

     if ((flags & (AVSEEK_FLAG_CHAPTER|AVSEEK_FLAG_BACKWARD)) == (AVSEEK_FLAG_CHAPTER|AVSEEK_FLAG_BACKWARD)) { 
         for (loop = 0; loop < pos; loop++) 
             res = dvdnav_prev_pg_search(dvd->nav_data); 
     } else if ((flags & AVSEEK_FLAG_CHAPTER) == AVSEEK_FLAG_CHAPTER) { 
         for (loop = 0; loop < pos; loop++) 
             res = dvdnav_next_pg_search(dvd->nav_data); 
     } else if (flags & AVSEEK_FLAG_DIRECT) { 
         // time 90000 to dvd time 
         res = dvdnav_time_search(dvd->nav_data, pos * 90000); 
     } 

    return 0;

}
*/
/* Seek in the dvd. Used for seeking in the vob (pts seek) itself */
static int64_t dvd_seek(URLContext *h, int64_t pos, int whence)
{
    dvdnav_status_t res;
    uint32_t pos2 = pos, len;

    /* get our data */
    DVDContext *dvd = (DVDContext *)h->priv_data;
    if (!dvd)
        return AVERROR(EFAULT);

    /* get current position */
    res = dvdnav_get_position(dvd->nav_data, &pos2, &len);
    av_log(h,0,"The length of the current block is %d\n",len);
    if (res != DVDNAV_STATUS_OK)
        return -1;

    switch (whence) {
    case SEEK_SET:
    case SEEK_CUR:
    case SEEK_END:
        if (whence == SEEK_END && pos2 == -1)
            return len * 2048;
        res = dvdnav_sector_search(dvd->nav_data, pos2 / 2048, whence);
        return pos2;
        break;

    case AVSEEK_SIZE:
        return len * 2048;
/*
    case AVSEEK_CHAPTER: 
    /*     /\* set the PGC positioning flag to have position information */
    /*      * relatively to whole dvd *\/ */
/*         if (dvdnav_set_PGC_positioning_flag(dvd->nav_data, 1) != DVDNAV_STATUS_OK) 
             return -1; 

         res = dvdnav_sector_search(dvd->nav_data, pos, SEEK_CUR); 
 */
    /*     /\* set the PGC positioning flag to have position information */
    /*      * relatively to whole chapter *\/ */
 /*        if (dvdnav_set_PGC_positioning_flag(dvd->nav_data, 0) != DVDNAV_STATUS_OK) 
             return -1; 
  */
    }

    return -1;
}

/* Select a button */
static void dvd_protocol_select_button(AVFormatContext *ctx, uint32_t index)
{
    URLContext *h = (ctx->pb)->opaque;
    DVDContext *dvd;
    pci_t *pci;

    if (!h || !h->priv_data)
        return;
    dvd = (DVDContext *)(h->priv_data);

    pci = dvdnav_get_current_nav_pci(dvd->nav_data);
    if (pci)
        dvdnav_button_select_and_activate(dvd->nav_data, pci, index+1);
}

/**
 * Say if the queue is empty.
 */
static void dvd_protocol_signal_queue(AVFormatContext *ctx)
{
    DVDContext *dvd;
    URLContext *h = (ctx->pb)->opaque;

    if (!h || !h->priv_data)
        return;

    dvd = (DVDContext *)h->priv_data;
    dvdnav_wait_skip(dvd->nav_data);
}


URLProtocol ff_dvd_protocol = {
    .name                = "dvd",
    .url_close           = dvd_close,
    .url_open            = dvd_open,
    .url_read            = dvd_read,
    .url_seek            = dvd_seek,
    .priv_data_size      = sizeof(DVDContext),
};
