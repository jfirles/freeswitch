/* 
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * 
 * Anthony Minessale II <anthm@freeswitch.org>
 *
 * mod_cv.cpp -- Detect Video motion
 *
 */


#include "opencv2/objdetect/objdetect.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"

using namespace std;
using namespace cv;

#include <switch.h>
#include <libyuv.h>

#include <cv.h>
#include "cvaux.h"
#include "cxmisc.h"
#include "highgui.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#define MY_EVENT_VIDEO_DETECT "cv::video_detect"

SWITCH_MODULE_LOAD_FUNCTION(mod_cv_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_cv_shutdown);
SWITCH_MODULE_DEFINITION(mod_cv, mod_cv_load, mod_cv_shutdown, NULL);


static const int NCHANNELS = 3;

struct detect_stats {
    uint32_t last_score;
    uint32_t simo_count;
    uint32_t simo_miss_count;
    uint32_t above_avg_simo_count;
    uint32_t sum;
    uint32_t itr;
    float avg;
};

struct shape {
    int x;
    int y;
    int x2;
    int y2;
    int w;
    int h;
    int cx;
    int cy;
    int radius;
};

#define MAX_SHAPES 32
#define MAX_OVERLAY 32

struct overlay {
    char *png_path;
    char *nick;
    switch_image_t *png;
    float x_off;
    float y_off;
    float shape_scale;
};

typedef struct cv_context_s {
    IplImage *rawImage;
    IplImage *yuvImage;
    CascadeClassifier *cascade;
    CascadeClassifier *nestedCascade;
    int w;
    int h;
    struct detect_stats detected;
    struct detect_stats nestDetected;
    int detect_event;
    int nest_detect_event;
    struct shape shape[MAX_SHAPES];
    int shape_idx;
    int32_t skip;
    int32_t skip_count;
    uint32_t debug;
    struct overlay overlay[MAX_OVERLAY];
    uint32_t overlay_idx;
    uint32_t overlay_count;
	switch_core_session_t *session;
    char *cascade_path;
    char *nested_cascade_path;
    switch_memory_pool_t *pool;
    switch_mutex_t *mutex;
    char *png_prefix;
} cv_context_t;


static int clear_overlay(cv_context_t *context, int idx)
{
    uint32_t i = context->overlay_count;
    switch_image_t *png;
    int r = -1, x;

    context->overlay[idx].png_path = NULL;
    context->overlay[idx].nick = NULL;
    switch_img_free(&context->overlay[idx].png);
    memset(&context->overlay[idx], 0, sizeof(struct overlay));
    context->overlay_count--;

    for (x = idx + 1; x < i; x++) {
        context->overlay[x-1] = context->overlay[x];
        memset(&context->overlay[x], 0, sizeof(struct overlay));
    }

    return idx - 1 > 0 ? idx -1 : 0;
}

static int add_overlay(cv_context_t *context, const char *png_path, const char *nick)
{
    uint32_t i = context->overlay_count;
    switch_image_t *png;
    int r = -1, x;

    for (x = 0; x < i; x++) {
        if (context->overlay[x].png_path) {
            if (!zstr(nick)) {
                if (!zstr(context->overlay[x].nick) && !strcmp(context->overlay[x].nick, nick)) {
                    return x;
                }
            } else {
                if (strstr(context->overlay[x].png_path, png_path)) {
                    return x;
                }
            }
        }
    }

    if (context->png_prefix) {
        context->overlay[i].png_path = switch_core_sprintf(context->pool, "%s%s%s", context->png_prefix, SWITCH_PATH_SEPARATOR, png_path);
    } else {
        context->overlay[i].png_path = switch_core_strdup(context->pool, png_path);
    }

    if ((png = switch_img_read_png(context->overlay[i].png_path, SWITCH_IMG_FMT_ARGB))) {
        context->overlay[i].png = png;
        if (!zstr(nick)) {
            context->overlay[i].nick = switch_core_strdup(context->pool, nick);
        }
        if (!context->overlay[i].shape_scale) context->overlay[i].shape_scale = 1;
        context->overlay_count++;
        r = (int) i;
    } else {
        context->overlay[i].png_path = NULL;
    }

    return r;
}


static void uninit_context(cv_context_t *context);

static const float coef1 = 0.3190;
static const float coef2 = -48.7187;

static void reset_stats(struct detect_stats *stats)
{
    memset(stats, 0, sizeof(*stats));
}


static void reset_context(cv_context_t *context)
{

    CascadeClassifier *cascade = context->cascade;
    CascadeClassifier *nestedCascade = context->nestedCascade;

    context->cascade = NULL;
    context->nestedCascade = NULL;

    if (cascade) {
        delete cascade;
    }

    if (nestedCascade) {
        delete nestedCascade;
    }

}

static void uninit_context(cv_context_t *context)
{
    int i = 0;

    reset_context(context);

    for (i = 0; i < context->overlay_count; i++) {
        switch_img_free(&context->overlay[i].png);
        context->overlay[i].png_path = NULL;
        context->overlay_count = 0;
        memset(&context->overlay[i], 0, sizeof(struct overlay));
    }

    switch_core_destroy_memory_pool(&context->pool);
}


static void init_context(cv_context_t *context)
{
    int create = 0;

    if (!context->pool) {
        switch_core_new_memory_pool(&context->pool);
        switch_mutex_init(&context->mutex, SWITCH_MUTEX_NESTED, context->pool);
        context->png_prefix = switch_core_get_variable_pdup("cv_png_prefix", context->pool);
        create = 1;
    }

    switch_mutex_lock(context->mutex);

    if (!create) {
        reset_context(context);
    }

    if (context->cascade_path) {
        context->cascade = new CascadeClassifier;
        context->cascade->load(context->cascade_path);

        if (context->nested_cascade_path) {
            context->nestedCascade = new CascadeClassifier;
            context->nestedCascade->load(context->nested_cascade_path);
        }
    }

    switch_mutex_unlock(context->mutex);
}



static void parse_stats(struct detect_stats *stats, uint32_t size, uint64_t skip)
{
    if (stats->itr >= 500) {
        reset_stats(stats);
    }
    
    if (stats->itr >= 60) {
        if (stats->last_score > stats->avg + 10) {
            stats->above_avg_simo_count += skip;
        } else if (stats->above_avg_simo_count) {
            stats->above_avg_simo_count = 0;
        }
    }


    if (size) {
        stats->simo_miss_count = 0;
        stats->simo_count += skip;
        stats->last_score = size;
        stats->sum += size;
    } else {
        stats->simo_miss_count += skip;
        stats->simo_count = 0;
        stats->itr = 0;
        stats->avg = 0;
    }

    stats->itr++;
    stats->avg = stats->sum / stats->itr;
}

void detectAndDraw(cv_context_t *context)
{
    double scale = 1;
    Mat img(context->rawImage);

    switch_mutex_lock(context->mutex);


    if (context->shape[0].cx && context->skip > 1 && context->skip_count++ < context->skip) {
        switch_mutex_unlock(context->mutex);
        return;
    }

    context->skip_count = 0;

    if (context->rawImage->width >= 1080) {
        scale = 2;
    } else if (context->rawImage->width >= 720) {
        scale = 1.5;
    }


    int i = 0;
    vector<Rect> detectedObjs, detectedObjs2;
    const static Scalar colors[] =  { CV_RGB(0,0,255),
                                      CV_RGB(0,128,255),
                                      CV_RGB(0,255,255),
                                      CV_RGB(0,255,0),
                                      CV_RGB(255,128,0),
                                      CV_RGB(255,255,0),
                                      CV_RGB(255,0,0),
                                      CV_RGB(255,0,255)} ;

    Mat gray, smallImg( cvRound (img.rows/scale), cvRound(img.cols/scale), CV_8UC1 );

    const int max_neighbors = MAX(0, cvRound((float)coef1*smallImg.cols + coef2));

    cvtColor( img, gray, CV_BGR2GRAY );
    resize( gray, smallImg, smallImg.size(), 0, 0, INTER_LINEAR );
    equalizeHist( smallImg, smallImg );

    context->cascade->detectMultiScale( smallImg, detectedObjs,
                                        1.1, 2, 0
                                        |CV_HAAR_FIND_BIGGEST_OBJECT
                                        |CV_HAAR_DO_ROUGH_SEARCH
                                        |CV_HAAR_SCALE_IMAGE
                                        ,
                                        Size(20, 20) );


    parse_stats(&context->detected, detectedObjs.size(), context->skip);

    //printf("SCORE: %d %f %d\n", context->detected.simo_count, context->detected.avg, context->detected.last_score);

    context->shape_idx = 0;
    //memset(context->shape, 0, sizeof(context->shape[0]) * MAX_SHAPES);

    for( vector<Rect>::iterator r = detectedObjs.begin(); r != detectedObjs.end(); r++, i++ ) {
        Mat smallImgROI;
        vector<Rect> nestedObjects;
        Point center;
        Scalar color = colors[i%8];
        int radius;
        
        double aspect_ratio = (double)r->width/r->height;

        if (context->shape_idx >= MAX_SHAPES) {
            break;
        }
        

        if(0.75 < aspect_ratio && aspect_ratio < 1.3 ) {
            center.x = switch_round_to_step(cvRound((r->x + r->width*0.5)*scale), 20);
            center.y = switch_round_to_step(cvRound((r->y + r->height*0.5)*scale), 20);
            radius = switch_round_to_step(cvRound((r->width + r->height)*0.25*scale), 20);

            if (context->debug || !context->overlay_count) {
                circle( img, center, radius, color, 3, 8, 0 );
            }

            context->shape[context->shape_idx].x = center.x - radius;
            context->shape[context->shape_idx].y = center.y - radius;
            context->shape[context->shape_idx].cx = center.x;
            context->shape[context->shape_idx].cy = center.y;
            context->shape[context->shape_idx].radius = radius;
            context->shape[context->shape_idx].w = context->shape[context->shape_idx].h = radius * 2;
            context->shape_idx++;

        } else {
            context->shape[context->shape_idx].x = switch_round_to_step(cvRound(r->x*scale), 40);
            context->shape[context->shape_idx].y = switch_round_to_step(cvRound(r->y*scale), 20);
            context->shape[context->shape_idx].x2 = switch_round_to_step(cvRound((r->x + r->width-1)*scale), 40);
            context->shape[context->shape_idx].y2 = switch_round_to_step(cvRound((r->y + r->height-1)*scale), 20);
            context->shape[context->shape_idx].w = context->shape[context->shape_idx].x2 - context->shape[context->shape_idx].x;
            context->shape[context->shape_idx].h = context->shape[context->shape_idx].y2 - context->shape[context->shape_idx].y;
            context->shape[context->shape_idx].cx = context->shape[context->shape_idx].x + (context->shape[context->shape_idx].w / 2);
            context->shape[context->shape_idx].cy = context->shape[context->shape_idx].y + (context->shape[context->shape_idx].h / 2);
            
            if (context->debug || !context->overlay_count) {
                rectangle( img, cvPoint(context->shape[context->shape_idx].x, context->shape[context->shape_idx].y),
                           cvPoint(context->shape[context->shape_idx].x2, context->shape[context->shape_idx].y2),
                           color, 3, 8, 0);
            }

            context->shape_idx++;
        }

        if(!context->nestedCascade || context->nestedCascade->empty() ) {
            continue;
        }

        const int half_height=cvRound((float)r->height/2);

        r->y = r->y + half_height;
        r->height = half_height;
        smallImgROI = smallImg(*r);
        context->nestedCascade->detectMultiScale( smallImgROI, nestedObjects,
                                                  1.1, 0, 0
                                                  //|CV_HAAR_FIND_BIGGEST_OBJECT
                                                  //|CV_HAAR_DO_ROUGH_SEARCH
                                                  //|CV_HAAR_DO_CANNY_PRUNING
                                                  |CV_HAAR_SCALE_IMAGE
                                                  ,
                                                  Size(30, 30) );
        


        // Draw rectangle reflecting confidence
        const int object_neighbors = nestedObjects.size();
        //cout << "Detected " << object_neighbors << " object neighbors" << endl;
        const int rect_height = cvRound((float)img.rows * object_neighbors / max_neighbors);
        CvScalar col = CV_RGB((float)255 * object_neighbors / max_neighbors, 0, 0);
        rectangle(img, cvPoint(0, img.rows), cvPoint(img.cols/10, img.rows - rect_height), col, -1);

        parse_stats(&context->nestDetected, nestedObjects.size(), context->skip);
        
        
        //printf("NEST: %d %f %d\n", context->nestDetected.simo_count, context->nestDetected.avg, context->nestDetected.last_score);
    }
    
    switch_mutex_unlock(context->mutex);
}


static switch_status_t video_thread_callback(switch_core_session_t *session, switch_frame_t *frame, void *user_data)
{
    cv_context_t *context = (cv_context_t *) user_data;
    switch_channel_t *channel = switch_core_session_get_channel(session);

    if (!switch_channel_ready(channel)) {
        return SWITCH_STATUS_FALSE;
    }

    if (frame->img) {
        if ((frame->img->d_w != context->w || frame->img->d_h != context->h) && context->rawImage) {
            cvReleaseImage(&context->rawImage);
            cvReleaseImage(&context->yuvImage);
        }

        if (!context->rawImage) {
            context->rawImage = cvCreateImage(cvSize(frame->img->d_w, frame->img->d_h), IPL_DEPTH_8U, 3);
            context->yuvImage = cvCreateImage(cvSize(frame->img->d_w, frame->img->d_h), IPL_DEPTH_8U, 3);
            switch_assert(context->rawImage);
            switch_assert(context->rawImage->width * 3 == context->rawImage->widthStep);
        }

        //printf("context->rawImage: %dx%d stride: %d size: %d color:%s\n", context->rawImage->width, context->rawImage->height, context->rawImage->widthStep, context->rawImage->imageSize, context->rawImage->colorModel);

        libyuv::I420ToRGB24(frame->img->planes[0], frame->img->stride[0],
                            frame->img->planes[1], frame->img->stride[1],
                            frame->img->planes[2], frame->img->stride[2],
                            (uint8_t *)context->rawImage->imageData, context->rawImage->widthStep,
                            context->rawImage->width, context->rawImage->height);


        if (context->cascade) {
            switch_event_t *event;

            detectAndDraw(context);
            
            if (context->detected.simo_count > 20) {
                if (!context->detect_event) {
                    context->detect_event = 1;
                    
                    if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, MY_EVENT_VIDEO_DETECT) == SWITCH_STATUS_SUCCESS) {
                        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Detect-Type", "primary");
                        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Detect-Disposition", "start");
                        switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Detect-Simo-Count", "%u", context->detected.simo_count);
                        switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Detect-Average", "%f", context->detected.avg);
                        switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Detect-Last-Score", "%u", context->detected.last_score);
                        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Unique-ID", switch_core_session_get_uuid(session));
                        //switch_channel_event_set_data(channel, event);
                        DUMP_EVENT(event);
                        switch_event_fire(&event);
                    }
                    
                    switch_channel_execute_on(channel, "execute_on_cv_detect_primary");
                    
                }
            } else {
                if (context->detected.simo_miss_count >= 20) {
                    if (context->detect_event) {
                        if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, MY_EVENT_VIDEO_DETECT) == SWITCH_STATUS_SUCCESS) {
                            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Detect-Type", "primary");
                            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Detect-Disposition", "stop");
                            switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Detect-Simo-Count", "%u", context->detected.simo_count);
                            switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Detect-Average", "%f", context->detected.avg);
                            switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Detect-Last-Score", "%u", context->detected.last_score);
                            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Unique-ID", switch_core_session_get_uuid(session));
                            //switch_channel_event_set_data(channel, event);
                            DUMP_EVENT(event);
                            switch_event_fire(&event);
                        }

                    
                        memset(context->shape, 0, sizeof(context->shape[0]) * MAX_SHAPES);

                        switch_channel_execute_on(channel, "execute_on_cv_detect_off_primary");
                        reset_stats(&context->nestDetected);
                        reset_stats(&context->detected);
                    }

                    context->detect_event = 0;
                }

            }

            if (context->nestedCascade && context->detected.simo_count > 20) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "CHECKING: %d %d %f %d\n", context->nestDetected.itr, context->nestDetected.last_score, context->nestDetected.avg, context->nestDetected.above_avg_simo_count);

                if (context->nestDetected.simo_count > 20 && context->nestDetected.last_score > context->nestDetected.avg && 
                    context->nestDetected.above_avg_simo_count > 5) {
                    if (!context->nest_detect_event) {
                        context->nest_detect_event = 1;

                        if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, MY_EVENT_VIDEO_DETECT) == SWITCH_STATUS_SUCCESS) {
                            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Detect-Type", "nested");
                            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Detect-Disposition", "start");
                            switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Detect-Simo-Count", "%d", context->nestDetected.simo_count);
                            switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Detect-Average", "%f", context->nestDetected.avg);
                            switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Detect-Last-Score", "%u", context->nestDetected.last_score);
                            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Unique-ID", switch_core_session_get_uuid(session));
                            //switch_channel_event_set_data(channel, event);
                            DUMP_EVENT(event);
                            switch_event_fire(&event);
                        }

                        switch_channel_execute_on(channel, "execute_on_cv_detect_nested");
                    }
                } else if (context->nestDetected.above_avg_simo_count == 0) {
                    if (context->nest_detect_event) {
                        if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, MY_EVENT_VIDEO_DETECT) == SWITCH_STATUS_SUCCESS) {
                            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Detect-Type", "nested");
                            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Detect-Disposition", "stop");
                            switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Detect-Simo-Count", "%d", context->nestDetected.simo_count);
                            switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Detect-Average", "%f", context->nestDetected.avg);
                            switch_event_add_header(event, SWITCH_STACK_BOTTOM, "Detect-Last-Score", "%u", context->nestDetected.last_score);
                            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Unique-ID", switch_core_session_get_uuid(session));
                            //switch_channel_event_set_data(channel, event);
                            DUMP_EVENT(event);
                            switch_event_fire(&event);
                        }
                        switch_channel_execute_on(channel, "execute_on_cv_detect_off_nested");
                        reset_stats(&context->nestDetected);
                    }
                    
                    context->nest_detect_event = 0;
                }
            }
        }

        int w = context->rawImage->width;
        int h = context->rawImage->height;

        if (context->debug) {
            libyuv::RGB24ToI420((uint8_t *)context->rawImage->imageData, w * 3,
                                frame->img->planes[0], frame->img->stride[0],
                                frame->img->planes[1], frame->img->stride[1],
                                frame->img->planes[2], frame->img->stride[2],
                                context->rawImage->width, context->rawImage->height);
        }

        if (context->overlay_count && context->detect_event && context->shape[0].cx) {
            int i;

            for (i = 0; i < context->overlay_count; i++) {
                struct overlay *overlay = &context->overlay[i];
                int x = 0, y = 0;
                switch_image_t *img = NULL;
                int scale_w = 0, scale_h = 0;
                int x_off = 0, y_off = 0;
                int shape_w, shape_h;
                int cx, cy;

                shape_w = context->shape[0].w;
                shape_h = context->shape[0].h;
            
                cx = context->shape[0].cx;
                cy = context->shape[0].cy;
            
                scale_w = shape_w * overlay->shape_scale;
                if (scale_w > frame->img->d_w) {
                    scale_w = frame->img->d_w;
                }
                scale_h = ((overlay->png->d_h * scale_w) / overlay->png->d_w);
                
                if (overlay->x_off) {
                    x_off = overlay->x_off * shape_w;
                }
                
                if (overlay->y_off) {
                    y_off = overlay->y_off * context->shape[0].h;
                }

                x = cx - ((scale_w / 2) + x_off);
                y = cy - ((scale_h / 2) + y_off);
            

                switch_img_scale(overlay->png, &img, scale_w, scale_h);

                if (img) {
                    switch_img_patch(frame->img, img, x, y);
                    switch_img_free(&img);
                }
            }

        } else if (!context->debug) {
            libyuv::RGB24ToI420((uint8_t *)context->rawImage->imageData, w * 3,
                                frame->img->planes[0], frame->img->stride[0],
                                frame->img->planes[1], frame->img->stride[1],
                                frame->img->planes[2], frame->img->stride[2],
                                context->rawImage->width, context->rawImage->height);
        }
        
    }

    return SWITCH_STATUS_SUCCESS;
}

static void parse_params(cv_context_t *context, int start, int argc, char **argv)
{
    int i, changed = 0, png_idx = 0, png_count = 0;
    char *nick = NULL;

    png_count = context->overlay_count;
    
    for (i = start; i < argc ; i ++) {
        char *name = strdup(argv[i]);
        char *val = NULL;

        if ((val = strchr(name, '='))) {
            *val++ = '\0';
        }

        if (name && val) {

            if (!strcasecmp(name, "x_off")) {
                context->overlay[png_idx].x_off = atof(val);
            } else if (!strcasecmp(name, "nick")) {
                switch_safe_free(nick);
                nick = strdup(val);
            } else if (!strcasecmp(name, "y_off")) {
                context->overlay[png_idx].y_off = atof(val);
            } else if (!strcasecmp(name, "scale")) {
                context->overlay[png_idx].shape_scale = atof(val);
            } else if (!strcasecmp(name, "skip")) {
                context->skip = atoi(val);
            } else if (!strcasecmp(name, "debug")) {
                context->debug = atoi(val);
            } else if (!strcasecmp(name, "cascade")) {
                context->cascade_path = switch_core_strdup(context->pool, val);
                changed++;
            } else if (!strcasecmp(name, "nested_cascade")) {
                context->nested_cascade_path = switch_core_strdup(context->pool, val);
                changed++;
            } else if (!strcasecmp(name, "png")) {
                png_idx = add_overlay(context, val, nick);
            }
        } else if (name) {
            if (!strcasecmp(name, "clear")) {
                png_idx = clear_overlay(context, png_idx);
            }
        }

        free(name);
    }

    switch_safe_free(nick);

    if (context->overlay_count != png_count) {
        changed++;
    }

    if (!context->skip) context->skip = 1;

    if (changed) {
        init_context(context);
    }
}




SWITCH_STANDARD_APP(cv_start_function)
{
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_frame_t *read_frame;
    cv_context_t context = { 0 };
    char *lbuf;
    char *cascade_path;
    char *nested_cascade_path;
	char *argv[25];
	int argc;

    init_context(&context);

	if (data && (lbuf = switch_core_session_strdup(session, data))
		&& (argc = switch_separate_string(lbuf, ' ', argv, (sizeof(argv) / sizeof(argv[0]))))) {
        context.cascade_path = argv[0];
        context.nested_cascade_path = argv[1];

        parse_params(&context, 2, argc, argv);
    }
    
    switch_channel_answer(channel);
    switch_channel_set_flag_recursive(channel, CF_VIDEO_DECODED_READ);
    switch_channel_set_flag_recursive(channel, CF_VIDEO_ECHO);

    switch_core_session_raw_read(session);

    switch_core_session_set_video_read_callback(session, video_thread_callback, &context);

	while (switch_channel_ready(channel)) {
		switch_status_t status = switch_core_session_read_frame(session, &read_frame, SWITCH_IO_FLAG_NONE, 0);

		if (!SWITCH_READ_ACCEPTABLE(status)) {
			break;
		}

		if (switch_test_flag(read_frame, SFF_CNG)) {
			continue;
		}

        memset(read_frame->data, 0, read_frame->datalen);
        switch_core_session_write_frame(session, read_frame, SWITCH_IO_FLAG_NONE, 0);
    }

    switch_core_session_set_video_read_callback(session, NULL, NULL);
    
    uninit_context(&context);

    switch_core_session_reset(session, SWITCH_TRUE, SWITCH_TRUE);
}


///////


static switch_bool_t cv_bug_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
    cv_context_t *context = (cv_context_t *) user_data;

    switch_channel_t *channel = switch_core_session_get_channel(context->session);

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		{
            switch_channel_set_flag_recursive(channel, CF_VIDEO_DECODED_READ);
		}
		break;
	case SWITCH_ABC_TYPE_CLOSE:
		{
            switch_channel_clear_flag_recursive(channel, CF_VIDEO_DECODED_READ);
            uninit_context(context);
		}
		break;
	case SWITCH_ABC_TYPE_READ_VIDEO_PING: 
        {
            switch_frame_t *frame = switch_core_media_bug_get_video_ping_frame(bug);
            video_thread_callback(context->session, frame, context);
        }
        break;
	default:
		break;
	}

	return SWITCH_TRUE;
}

SWITCH_STANDARD_APP(cv_bug_start_function)
{
	switch_media_bug_t *bug;
	switch_status_t status;
	switch_channel_t *channel = switch_core_session_get_channel(session);
    cv_context_t *context;
	char *lbuf = NULL;
	int x, n;
	char *argv[25] = { 0 };
	int argc;

	if ((bug = (switch_media_bug_t *) switch_channel_get_private(channel, "_cv_bug_"))) {
		if (!zstr(data) && !strcasecmp(data, "stop")) {
			switch_channel_set_private(channel, "_cv_bug_", NULL);
			switch_core_media_bug_remove(session, &bug);
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Cannot run 2 at once on the same channel!\n");
		}
		return;
	}

	context = (cv_context_t *) switch_core_session_alloc(session, sizeof(*context));
	assert(context != NULL);
    context->session = session;

    init_context(context);
    
	if (data && (lbuf = switch_core_session_strdup(session, data))
		&& (argc = switch_separate_string(lbuf, ' ', argv, (sizeof(argv) / sizeof(argv[0]))))) {
        parse_params(context, 1, argc, argv);
    }

	if ((status = switch_core_media_bug_add(session, "cv_bug", NULL, cv_bug_callback, context, 0, SMBF_READ_VIDEO_PING, &bug)) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failure!\n");
		return;
	}

	switch_channel_set_private(channel, "_cv_bug_", bug);

}

/* API Interface Function */
#define CV_BUG_API_SYNTAX "<uuid> [start|stop]"
SWITCH_STANDARD_API(cv_bug_api_function)
{
	switch_core_session_t *rsession = NULL;
	switch_channel_t *channel = NULL;
	switch_media_bug_t *bug;
	switch_status_t status;
    cv_context_t *context;
	char *mycmd = NULL;
	int argc = 0;
	char *argv[25] = { 0 };
	char *uuid = NULL;
	char *action = NULL;
    char *cascade_path = NULL;
    char *nested_cascade_path = NULL;
	char *lbuf = NULL;
	int x, n, i;

	if (zstr(cmd)) {
		goto usage;
	}

	if (!(mycmd = strdup(cmd))) {
		goto usage;
	}

	if ((argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])))) < 2) {
		goto usage;
	}

	uuid = argv[0];
	action = argv[1];

	if (!(rsession = switch_core_session_locate(uuid))) {
		stream->write_function(stream, "-ERR Cannot locate session!\n");
		goto done;
	}

	channel = switch_core_session_get_channel(rsession);

	if ((bug = (switch_media_bug_t *) switch_channel_get_private(channel, "_cv_bug_"))) {
		if (!zstr(action)) {
            if (!strcasecmp(action, "stop")) {
                switch_channel_set_private(channel, "_cv_bug_", NULL);
                switch_core_media_bug_remove(rsession, &bug);
                stream->write_function(stream, "+OK Success\n");
            } else if (!strcasecmp(action, "start") || !strcasecmp(action, "mod")) {
                context = (cv_context_t *) switch_core_media_bug_get_user_data(bug);
                switch_assert(context);
                parse_params(context, 2, argc, argv);
                stream->write_function(stream, "+OK Success\n");
            }
        } else {
            stream->write_function(stream, "-ERR Invalid action\n");
        }
        goto done;
	}

	if (!zstr(action) && strcasecmp(action, "start")) {
		goto usage;
	}

	context = (cv_context_t *) switch_core_session_alloc(rsession, sizeof(*context));
	assert(context != NULL);
	context->session = rsession;

    init_context(context);
    parse_params(context, 2, argc, argv);

	if ((status = switch_core_media_bug_add(rsession, "cv_bug", NULL, cv_bug_callback, context, 0, SMBF_READ_VIDEO_PING, &bug)) != SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "-ERR Failure!\n");
		goto done;
	} else {
		switch_channel_set_private(channel, "_cv_bug_", bug);
		stream->write_function(stream, "+OK Success\n");
		goto done;
	}


 usage:
	stream->write_function(stream, "-USAGE: %s\n", CV_BUG_API_SYNTAX);

 done:
	if (rsession) {
		switch_core_session_rwunlock(rsession);
	}

	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

///////


SWITCH_MODULE_LOAD_FUNCTION(mod_cv_load)
{
    switch_application_interface_t *app_interface;
	switch_api_interface_t *api_interface;

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	SWITCH_ADD_APP(app_interface, "cv", "", "", cv_start_function, "", SAF_NONE);

	SWITCH_ADD_APP(app_interface, "cv_bug", "connect cv", "connect cv", 
                   cv_bug_start_function, "[</path/to/haar.xml>]", SAF_NONE);

	SWITCH_ADD_API(api_interface, "cv_bug", "cv_bug", cv_bug_api_function, CV_BUG_API_SYNTAX);

	switch_console_set_complete("add cv_bug ::console::list_uuid ::[start:stop");


    
	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_cv_shutdown)
{
    return SWITCH_STATUS_UNLOAD;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:nil
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */