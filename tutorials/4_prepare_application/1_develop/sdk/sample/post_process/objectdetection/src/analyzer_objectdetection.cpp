/****************************************************************************
 * Copyright 2023 Sony Semiconductor Solutions Corp. All rights reserved.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <vector>

#include "vision_app_public.h"
#include "analyzer_objectdetection.h"
#include "objectdetection_generated.h"

extern pthread_mutex_t g_libc_mutex;

/* -------------------------------------------------------- */
/* define                                                   */
/* -------------------------------------------------------- */
#define PPL_DNN_OUTPUT_TENSOR_SIZE(dnnOutputDetections, dnnOutputElements)  (dnnOutputDetections * dnnOutputElements)    // bbox(dnnOutputDetections*4)+class(dnnOutputDetections)+scores(dnnOutputDetections)+numOfDetections(1) 

/* -------------------------------------------------------- */
/* macro define                                             */
/* -------------------------------------------------------- */
#define PPL_ERR_PRINTF(fmt, ...) pthread_mutex_lock(&g_libc_mutex);fprintf(stderr, "E [VisionAPP] ");fprintf(stderr, fmt, ##__VA_ARGS__);fprintf(stderr, "\n");pthread_mutex_unlock(&g_libc_mutex);
#define PPL_WARN_PRINTF(fmt, ...) pthread_mutex_lock(&g_libc_mutex);fprintf(stderr, "W [VisionAPP] ");fprintf(stderr, fmt, ##__VA_ARGS__);fprintf(stderr, "\n");pthread_mutex_unlock(&g_libc_mutex);
#define PPL_INFO_PRINTF(fmt, ...) pthread_mutex_lock(&g_libc_mutex);fprintf(stdout, "I [VisionAPP] ");fprintf(stdout, fmt, ##__VA_ARGS__);fprintf(stdout, "\n");pthread_mutex_unlock(&g_libc_mutex);
#define PPL_DBG_PRINTF(fmt, ...) pthread_mutex_lock(&g_libc_mutex);printf( "D [VisionAPP] "); printf( fmt, ##__VA_ARGS__); printf( "\n");pthread_mutex_unlock(&g_libc_mutex);
#define PPL_VER_PRINTF(fmt, ...) pthread_mutex_lock(&g_libc_mutex);printf( "V [VisionAPP] "); printf( fmt, ##__VA_ARGS__); printf( "\n");pthread_mutex_unlock(&g_libc_mutex);

/* -------------------------------------------------------- */
/* structure                                                */
/* -------------------------------------------------------- */
typedef struct tagBbox {
    float x_min;
    float y_min;
    float x_max;
    float y_max;
} Bbox;

typedef struct tagObjectDetectionSsdOutputTensor {
    float numOfDetections;
    std::vector<Bbox> bboxes;
    std::vector<float> scores;
    std::vector<float> classes;
} ObjectDetectionSsdOutputTensor;

typedef struct tagPPL_Bbox {
    uint16_t    m_xmin;
    uint16_t    m_ymin;
    uint16_t    m_xmax;
    uint16_t    m_ymax;
} PPL_Bbox;

typedef struct OutputTensorForSort {
    PPL_Bbox s_bbox;
    float s_score;
    float s_class;
} OutputTensorForSort;

typedef struct tagObjectDetectionSsdData {
    uint8_t numOfDetections = 0;
    std::vector<PPL_Bbox> v_bbox;
    std::vector<float> v_scores;
    std::vector<uint8_t> v_classes;
} ObjectDetectionSsdData;

/* -------------------------------------------------------- */
/* static                                                   */
/* -------------------------------------------------------- */
static int createObjectDetectionSsdData(float *data_body, uint32_t detect_num, uint32_t element_num, ObjectDetectionSsdOutputTensor *objectdetection_output);
static void analyzeObjectDetectionSsdOutput(ObjectDetectionSsdOutputTensor out_tensor, ObjectDetectionSsdData *output_objectdetection_data, PPL_SsdParam ssd_param);
static void createSSDOutputFlatbuffer(flatbuffers::FlatBufferBuilder *builder, const ObjectDetectionSsdData *ssdData);
static EPPL_RESULT_CODE PPL_ObjectDetectionSsdParamInit(JSON_Value *root_value, PPL_SsdParam *p_ssd_param);

/* -------------------------------------------------------- */
/* public function                                          */
/* -------------------------------------------------------- */
/**
 * @brief PPL_ObjectDetectionSsdAnalyze
 */

bool o_tensor_cmp(const OutputTensorForSort& p, const OutputTensorForSort& q) {
    return p.s_score > q.s_score;
}

float my_iou(const PPL_Bbox& box1, const PPL_Bbox& box2, float eps = 1e-7) {
    float intersect = std::max(0, std::min(box1.m_xmax, box2.m_xmax) - std::max(box1.m_xmin, box2.m_xmin)) * std::max(0, std::min(box1.m_ymax, box2.m_ymax) - std::max(box1.m_ymin, box2.m_ymin));

    float union_area = (box1.m_xmax - box1.m_xmin) * (box1.m_ymax - box1.m_ymin) + (box2.m_xmax - box2.m_xmin) * (box2.m_ymax - box2.m_ymin) - intersect;

    return intersect / (union_area + eps);
}

EPPL_RESULT_CODE PPL_ObjectDetectionSsdAnalyze(float *p_data, uint32_t in_size, void **pp_out_buf,  uint32_t *p_out_size, PPL_SsdParam ssd_param) {

    uint8_t *p_out_param = NULL;
    ObjectDetectionSsdOutputTensor objectdetection_output;
    ObjectDetectionSsdData output_objectdetection_data;

    PPL_DBG_PRINTF("PPL_ObjectDetectionSsdAnalyze");

    if (p_data == NULL) {
        PPL_ERR_PRINTF("PPL:Invalid param : pdata=NULL");
        return E_PPL_INVALID_PARAM;
    }

    if (in_size != (uint32_t)(PPL_DNN_OUTPUT_TENSOR_SIZE(ssd_param.dnnOutputDetections, ssd_param.dnnOutputElements))) {
        PPL_ERR_PRINTF("PPL:Unexpected value for : in_size %d",in_size);
        PPL_ERR_PRINTF("PPL:Expected value for : in_size %d",(uint32_t)(PPL_DNN_OUTPUT_TENSOR_SIZE(ssd_param.dnnOutputDetections, ssd_param.dnnOutputElements)));
        return E_PPL_INVALID_PARAM;
    }

    /* call interface process */
    int ret = createObjectDetectionSsdData(p_data, ssd_param.dnnOutputDetections, ssd_param.dnnOutputElements, &objectdetection_output);
    if (ret != 0) {
        PPL_ERR_PRINTF("PPL: Error in createObjectDetectionData");
        return E_PPL_OTHER;
    }

    /* call analyze process */
    analyzeObjectDetectionSsdOutput(objectdetection_output, &output_objectdetection_data, ssd_param);

    /* Serialize Data to FLatbuffers */ 
    pthread_mutex_lock(&g_libc_mutex);
    flatbuffers::FlatBufferBuilder* builder = new flatbuffers::FlatBufferBuilder();
    pthread_mutex_unlock(&g_libc_mutex);

    createSSDOutputFlatbuffer(builder,&output_objectdetection_data);

    pthread_mutex_lock(&g_libc_mutex);
    uint8_t* buf_ptr = builder->GetBufferPointer();
    pthread_mutex_unlock(&g_libc_mutex);

    if (buf_ptr == NULL) {
        PPL_ERR_PRINTF("Error could not create Flatbuffer");

        pthread_mutex_lock(&g_libc_mutex);
        builder->Clear();
        pthread_mutex_unlock(&g_libc_mutex);

        pthread_mutex_lock(&g_libc_mutex);
        delete(builder);
        pthread_mutex_unlock(&g_libc_mutex);

        return E_PPL_OTHER;
    }

    pthread_mutex_lock(&g_libc_mutex);
    uint32_t buf_size = builder->GetSize();
    pthread_mutex_unlock(&g_libc_mutex);

    pthread_mutex_lock(&g_libc_mutex);
    p_out_param = (uint8_t *)SessMalloc(buf_size);
    pthread_mutex_unlock(&g_libc_mutex);

    if (p_out_param == NULL) {
        PPL_ERR_PRINTF("malloc failed for creating flatbuffer, malloc size=%d", buf_size);
        pthread_mutex_lock(&g_libc_mutex);
        builder->Clear();
        pthread_mutex_unlock(&g_libc_mutex);

        pthread_mutex_lock(&g_libc_mutex);
        delete(builder);
        pthread_mutex_unlock(&g_libc_mutex);

        return E_PPL_E_MEMORY_ERROR;
    }
    PPL_DBG_PRINTF("p_out_param = %p", p_out_param);

    /* Copy Data */
    pthread_mutex_lock(&g_libc_mutex);
    memcpy(p_out_param, buf_ptr, buf_size);
    pthread_mutex_unlock(&g_libc_mutex);

    *pp_out_buf = p_out_param;
    *p_out_size = buf_size;

    //Clean up
    pthread_mutex_lock(&g_libc_mutex);
    builder->Clear();
    pthread_mutex_unlock(&g_libc_mutex);

    pthread_mutex_lock(&g_libc_mutex);
    delete(builder);
    pthread_mutex_unlock(&g_libc_mutex);

    return E_PPL_OK;
}

/**
 * json_parse: get json data from json_object and update in local struct
 * 
 * @param param Pointer to json string containing PPL specific parameters
 * @return Success or failure EPPL_RESULT_CODE
 */
EPPL_RESULT_CODE json_parse(JSON_Value *root_value, PPL_SsdParam *p_ssd_param) {
    PPL_DBG_PRINTF("PPL_Initialize: <json_parse>");
    return PPL_ObjectDetectionSsdParamInit(root_value, p_ssd_param);
}

/* -------------------------------------------------------- */
/* private function                                         */
/* -------------------------------------------------------- */
static EPPL_RESULT_CODE PPL_ObjectDetectionSsdParamInit(JSON_Value *root_value, PPL_SsdParam *p_ssd_param) {
    int ret;

    pthread_mutex_lock(&g_libc_mutex);
    ret = json_object_has_value(json_object(root_value),"dnn_output_detections");
    pthread_mutex_unlock(&g_libc_mutex);

    if (ret) {
        pthread_mutex_lock(&g_libc_mutex);
        uint16_t dnn_output_detections = json_object_get_number(json_object(root_value), "dnn_output_detections");
        pthread_mutex_unlock(&g_libc_mutex);

        PPL_DBG_PRINTF("PPL_ObjectDetectionParamInit dnn_output_detections: %d", dnn_output_detections);
        p_ssd_param->dnnOutputDetections = dnn_output_detections;
    } else {
        PPL_ERR_PRINTF("PPL_ObjectDetectionParamInit: json file does not have parameter \"dnn_output_detections\"");
        return E_PPL_INVALID_PARAM;
    }

    pthread_mutex_lock(&g_libc_mutex);
    ret = json_object_has_value(json_object(root_value),"dnn_output_elements");
    pthread_mutex_unlock(&g_libc_mutex);

    if (ret) {
        pthread_mutex_lock(&g_libc_mutex);
        uint16_t dnn_output_elements = json_object_get_number(json_object(root_value), "dnn_output_elements");
        pthread_mutex_unlock(&g_libc_mutex);

        PPL_DBG_PRINTF("PPL_ObjectDetectionParamInit dnn_output_elements: %d", dnn_output_elements);
        p_ssd_param->dnnOutputElements = dnn_output_elements;
    } else {
        PPL_ERR_PRINTF("PPL_ObjectDetectionParamInit: json file does not have parameter \"dnn_output_elements\"");
        return E_PPL_INVALID_PARAM;
    }

    pthread_mutex_lock(&g_libc_mutex);
    ret = json_object_has_value(json_object(root_value),"max_detections");
    pthread_mutex_unlock(&g_libc_mutex);

    if (ret) {
        pthread_mutex_lock(&g_libc_mutex);
        uint16_t maxDetections = (int)json_object_get_number(json_object(root_value), "max_detections");
        pthread_mutex_unlock(&g_libc_mutex);

        PPL_DBG_PRINTF("PPL_ObjectDetectionParamInit max_detections: %d", maxDetections);
        if (maxDetections > p_ssd_param->dnnOutputDetections) {
            PPL_WARN_PRINTF("max_detections > max number of classes, set to max number of classes");
            p_ssd_param->maxDetections = p_ssd_param->dnnOutputDetections;
        } else {
            p_ssd_param->maxDetections = maxDetections;
        }
    } else {
        PPL_ERR_PRINTF("PPL_ObjectDetectionParamInit json file does not have max_detections");
        return E_PPL_INVALID_PARAM;
    }

    pthread_mutex_lock(&g_libc_mutex);
    json_object_has_value(json_object(root_value),"threshold");
    pthread_mutex_unlock(&g_libc_mutex);

    if (ret) {
        pthread_mutex_lock(&g_libc_mutex);
        float threshold = json_object_get_number(json_object(root_value), "threshold");
        pthread_mutex_unlock(&g_libc_mutex);

        PPL_DBG_PRINTF("PPL_ObjectDetectionParamInit threshold: %lf", threshold);
        if (threshold < 0.0 || threshold > 1.0) {
            PPL_WARN_PRINTF("threshold value out of range, set to default threshold");
            p_ssd_param->threshold = PPL_DEFAULT_THRESHOLD;
        } else {
            p_ssd_param->threshold = threshold;
        }
    } else {
        PPL_ERR_PRINTF("PPL_ObjectDetectionParamInit: json file does not have threshold");
        return E_PPL_INVALID_PARAM;
    }
    
    pthread_mutex_lock(&g_libc_mutex);
    json_object_has_value(json_object(root_value),"iou_thres");
    pthread_mutex_unlock(&g_libc_mutex);

    if (ret) {
        pthread_mutex_lock(&g_libc_mutex);
        float iou_thres = json_object_get_number(json_object(root_value), "iou_thres");
        pthread_mutex_unlock(&g_libc_mutex);

        PPL_DBG_PRINTF("PPL_ObjectDetectionParamInit iou_thres: %lf", iou_thres);
        if (iou_thres < 0.0 || iou_thres > 1.0) {
            PPL_WARN_PRINTF("iou_thres value out of range, set to default iou_thres");
            p_ssd_param->iou_thres = PPL_IOU_THRESHOLD;
        } else {
            p_ssd_param->iou_thres = iou_thres;
        }
    } else {
        PPL_ERR_PRINTF("PPL_ObjectDetectionParamInit: json file does not have iou_thres");
        return E_PPL_INVALID_PARAM;
    }

    pthread_mutex_lock(&g_libc_mutex);
    ret = json_object_has_value(json_object(root_value),"input_width");
    pthread_mutex_unlock(&g_libc_mutex);

    if (ret) {
        pthread_mutex_lock(&g_libc_mutex);
        uint16_t input_width = json_object_get_number(json_object(root_value), "input_width");
        pthread_mutex_unlock(&g_libc_mutex);

        PPL_DBG_PRINTF("PPL_ObjectDetectionParamInit input_width: %d", input_width);
        p_ssd_param->inputWidth = input_width;
    } else {
        PPL_ERR_PRINTF("PPL_ObjectDetectionParamInit: json file does not have input_width");
        return E_PPL_INVALID_PARAM;
    }

    pthread_mutex_lock(&g_libc_mutex);
    ret = json_object_has_value(json_object(root_value),"input_height");
    pthread_mutex_unlock(&g_libc_mutex);

    if (ret) {
        pthread_mutex_lock(&g_libc_mutex);
        uint16_t input_height = json_object_get_number(json_object(root_value), "input_height");
        pthread_mutex_unlock(&g_libc_mutex);

        PPL_DBG_PRINTF("PPL_ObjectDetectionParamInit input_height: %d", input_height);
        p_ssd_param->inputHeight = input_height;
    } else {
        PPL_ERR_PRINTF("PPL_ObjectDetectionParamInit: json file does not have \"input_height\"");
        return E_PPL_INVALID_PARAM;
    }

    return E_PPL_OK;
}

/**
 * @brief createObjectDetectionSsdData
 */
static int createObjectDetectionSsdData(float *data_body, uint32_t detect_num, uint32_t element_num, ObjectDetectionSsdOutputTensor *objectdetection_output) {

    float* out_data = data_body;
    uint32_t count = 0;
    std::vector<Bbox> v_bbox;
    std::vector<float> v_scores;
    std::vector<float> v_classes;

    /* Extract number of Detections */
    uint16_t totalDetections = (uint16_t)detect_num;
    uint8_t totalElements = (uint8_t)element_num;
    /*
    if (((totalDetections * element_num))> PPL_DNN_OUTPUT_TENSOR_SIZE(detect_num)) {
        PPL_ERR_PRINTF("Invalid count index %d",count);
        return -1;
    }*/

    //Extract bounding box co-ordinates
    for (uint16_t i = 0; i < totalDetections; i++) {
        Bbox bbox;
        bbox.x_min = out_data[i] - out_data[i + totalDetections * 2]/2;
        bbox.y_min = out_data[i + totalDetections] - out_data[i + totalDetections * 3]/2;
        bbox.x_max = out_data[i] + out_data[i + totalDetections * 2]/2;
        bbox.y_max = out_data[i + totalDetections] + out_data[i + totalDetections * 3]/2;
        pthread_mutex_lock(&g_libc_mutex);
        v_bbox.push_back(bbox);
        pthread_mutex_unlock(&g_libc_mutex);
    }
    //count += (totalDetections * element_num);

    //Extract class indices
    for (uint16_t i = 0; i < totalDetections; i++) {
        /*if (count > PPL_DNN_OUTPUT_TENSOR_SIZE(detect_num)) {
            PPL_ERR_PRINTF("Invalid count index %d",count);
            return -1;
        }*/
        float class_index = 0;
        float score;
        float max_value;

        max_value = out_data[i + totalDetections * 5];


        for (uint8_t j = 0; j < element_num - 5; j++){
            if (out_data[i + totalDetections * (5 + j)] > max_value){
                max_value = out_data[i + totalDetections * (5 + j)];
                class_index = j;
            }
        }
        score = max_value;
        score *= out_data[i + totalDetections * 4];

        pthread_mutex_lock(&g_libc_mutex);
        v_classes.push_back(class_index);
        pthread_mutex_unlock(&g_libc_mutex);

        pthread_mutex_lock(&g_libc_mutex);
        v_scores.push_back(score);
        pthread_mutex_unlock(&g_libc_mutex);

        count++;
    }

    //Extract scores
    /*for (uint8_t i = 0; i < totalDetections; i++) {
        if (count > PPL_DNN_OUTPUT_TENSOR_SIZE(detect_num)) {
            PPL_ERR_PRINTF("Invalid count index %d",count);
            return -1;
        }
        float score;
        score = out_data[count];

        pthread_mutex_lock(&g_libc_mutex);
        v_scores.push_back(score);
        pthread_mutex_unlock(&g_libc_mutex);

        //count++;
    }*/

    /*if (count > PPL_DNN_OUTPUT_TENSOR_SIZE(detect_num)) {
        PPL_ERR_PRINTF("Invalid count index %d",count);
        return -1;
    }*/

    //Extract number of Detections
    uint16_t numOfDetections = (uint16_t) count;

    if (numOfDetections > totalDetections) {
         PPL_WARN_PRINTF("Unexpected value for numOfDetections: %d, setting it to %d",numOfDetections,totalDetections);
         numOfDetections = totalDetections;
    }

    objectdetection_output->numOfDetections = numOfDetections;

    pthread_mutex_lock(&g_libc_mutex);
    objectdetection_output->bboxes = v_bbox;
    pthread_mutex_unlock(&g_libc_mutex);

    pthread_mutex_lock(&g_libc_mutex);
    objectdetection_output->scores = v_scores;
    pthread_mutex_unlock(&g_libc_mutex);

    pthread_mutex_lock(&g_libc_mutex);
    objectdetection_output->classes = v_classes;
    pthread_mutex_unlock(&g_libc_mutex);

    return 0;
}

/**
 * @brief analyzeObjectDetectionSsdOutput
 */
static void analyzeObjectDetectionSsdOutput(ObjectDetectionSsdOutputTensor out_tensor, ObjectDetectionSsdData *output_objectdetection_data, PPL_SsdParam ssd_param) {

    uint16_t num_of_detections;
    uint16_t detections_above_threshold = 0;
    std::vector<PPL_Bbox> v_bbox;
    std::vector<float> v_scores;
    std::vector<uint8_t> v_classes;
    std::vector<PPL_Bbox> nms_bbox;
    std::vector<float> nms_scores;
    std::vector<uint8_t> nms_classes;
    ObjectDetectionSsdData objectdetection_data;

    std::vector<OutputTensorForSort> o_tensor_sort;

    /* Extract number of detections */
    num_of_detections = (uint16_t) out_tensor.numOfDetections;

    for (uint16_t i = 0; i < num_of_detections; i++) {

        /* Extract scores */
        float score;

        pthread_mutex_lock(&g_libc_mutex);
        score = out_tensor.scores[i];
        pthread_mutex_unlock(&g_libc_mutex);

        /* Filter Detections */
        if (score < ssd_param.threshold) {
            continue;
        } else {
            pthread_mutex_lock(&g_libc_mutex);
            v_scores.push_back(score);
            pthread_mutex_unlock(&g_libc_mutex);

            /* Extract bounding box co-ordinates */
            PPL_Bbox bbox;
            pthread_mutex_lock(&g_libc_mutex);
            bbox.m_xmin = (uint16_t)(round((out_tensor.bboxes[i].x_min) * (ssd_param.inputWidth - 1)));
            pthread_mutex_unlock(&g_libc_mutex);

            pthread_mutex_lock(&g_libc_mutex);
            bbox.m_ymin = (uint16_t)(round((out_tensor.bboxes[i].y_min) * (ssd_param.inputHeight  - 1)));
            pthread_mutex_unlock(&g_libc_mutex);

            pthread_mutex_lock(&g_libc_mutex);
            bbox.m_xmax = (uint16_t)(round((out_tensor.bboxes[i].x_max) * (ssd_param.inputWidth  - 1)));
            pthread_mutex_unlock(&g_libc_mutex);

            pthread_mutex_lock(&g_libc_mutex);
            bbox.m_ymax = (uint16_t)(round((out_tensor.bboxes[i].y_max) * (ssd_param.inputHeight - 1)));
            pthread_mutex_unlock(&g_libc_mutex);

            pthread_mutex_lock(&g_libc_mutex);
            v_bbox.push_back(bbox);
            pthread_mutex_unlock(&g_libc_mutex);

           /* Extract classes */
            uint8_t class_index;
            pthread_mutex_lock(&g_libc_mutex);
            class_index = (uint8_t)out_tensor.classes[i];
            pthread_mutex_unlock(&g_libc_mutex);

            pthread_mutex_lock(&g_libc_mutex);
            v_classes.push_back(class_index);
            pthread_mutex_unlock(&g_libc_mutex);

            detections_above_threshold++;
        }
    }

    for (uint16_t i = 0; i < detections_above_threshold; i++) {
        OutputTensorForSort tensor;
        tensor.s_bbox = v_bbox[i];
        tensor.s_score = v_scores[i];
        tensor.s_class = v_classes[i];
        pthread_mutex_lock(&g_libc_mutex);
        o_tensor_sort.push_back(tensor);
        pthread_mutex_unlock(&g_libc_mutex);   
    }

    sort(o_tensor_sort.begin(), o_tensor_sort.end(), o_tensor_cmp);

    uint16_t num = o_tensor_sort.size();
    std::vector<uint16_t> flg(num, 1);
    //std::vector<uint8_t> ret;

    for (uint16_t i = 0; i < num; ++i) {
        if (flg[i] == 0) {
            continue;
        }

        //ret.push_back(i);

        for (uint16_t j = i + 1; j < num; ++j) {
            if (flg[j] != 1) {
                continue;
            }
            if (o_tensor_sort[i].s_class == o_tensor_sort[j].s_class){

                float iou = my_iou(o_tensor_sort[i].s_bbox, o_tensor_sort[j].s_bbox);

                // Non-maximum suppression
                if (iou > ssd_param.iou_thres) {
                    flg[j] = 0;

                    pthread_mutex_lock(&g_libc_mutex);
                    o_tensor_sort[j].s_score = 0;
                    pthread_mutex_unlock(&g_libc_mutex);
                }
            }
        }
        pthread_mutex_lock(&g_libc_mutex);
        nms_bbox.push_back(o_tensor_sort[i].s_bbox);
        pthread_mutex_unlock(&g_libc_mutex);

        pthread_mutex_lock(&g_libc_mutex);
        nms_scores.push_back(o_tensor_sort[i].s_score);
        pthread_mutex_unlock(&g_libc_mutex);

        pthread_mutex_lock(&g_libc_mutex);
        nms_classes.push_back(o_tensor_sort[i].s_class);
        pthread_mutex_unlock(&g_libc_mutex);
    }

    uint16_t nms_numofdetection = nms_scores.size();

    objectdetection_data.numOfDetections = nms_numofdetection;

    pthread_mutex_lock(&g_libc_mutex);
    objectdetection_data.v_bbox = nms_bbox;
    pthread_mutex_unlock(&g_libc_mutex);

    pthread_mutex_lock(&g_libc_mutex);
    objectdetection_data.v_scores = nms_scores;
    pthread_mutex_unlock(&g_libc_mutex);

    pthread_mutex_lock(&g_libc_mutex);
    objectdetection_data.v_classes = nms_classes;
    pthread_mutex_unlock(&g_libc_mutex);

    //objectdetection_data = getActualDetections(objectdetection_data);

    if (objectdetection_data.numOfDetections > ssd_param.maxDetections) {
        objectdetection_data.numOfDetections = ssd_param.maxDetections;

        pthread_mutex_lock(&g_libc_mutex);
        objectdetection_data.v_bbox.resize(ssd_param.maxDetections);
        pthread_mutex_unlock(&g_libc_mutex);

        pthread_mutex_lock(&g_libc_mutex);
        objectdetection_data.v_classes.resize(ssd_param.maxDetections);
        pthread_mutex_unlock(&g_libc_mutex);

        pthread_mutex_lock(&g_libc_mutex);
        objectdetection_data.v_scores.resize(ssd_param.maxDetections);
        pthread_mutex_unlock(&g_libc_mutex);
    }

    output_objectdetection_data->numOfDetections = objectdetection_data.numOfDetections;

    pthread_mutex_lock(&g_libc_mutex);
    output_objectdetection_data->v_bbox = objectdetection_data.v_bbox;
    pthread_mutex_unlock(&g_libc_mutex);

    pthread_mutex_lock(&g_libc_mutex);
    output_objectdetection_data->v_scores = objectdetection_data.v_scores;
    pthread_mutex_unlock(&g_libc_mutex);

    pthread_mutex_lock(&g_libc_mutex);
    output_objectdetection_data->v_classes = objectdetection_data.v_classes;
    pthread_mutex_unlock(&g_libc_mutex);

    PPL_DBG_PRINTF("number of detections = %d", objectdetection_data.numOfDetections);
    num_of_detections = objectdetection_data.numOfDetections;
    for (int i = 0; i < num_of_detections; i++) {
        pthread_mutex_lock(&g_libc_mutex);
        uint16_t xmin = objectdetection_data.v_bbox[i].m_xmin;
        pthread_mutex_unlock(&g_libc_mutex);

        pthread_mutex_lock(&g_libc_mutex);
        uint16_t ymin = objectdetection_data.v_bbox[i].m_ymin;
        pthread_mutex_unlock(&g_libc_mutex);

        pthread_mutex_lock(&g_libc_mutex);
        uint16_t xmax = objectdetection_data.v_bbox[i].m_xmax;
        pthread_mutex_unlock(&g_libc_mutex);

        pthread_mutex_lock(&g_libc_mutex);
        uint16_t ymax = objectdetection_data.v_bbox[i].m_ymax;
        pthread_mutex_unlock(&g_libc_mutex);

        PPL_DBG_PRINTF("v_bbox[%d] :[x_min,y_min,x_max,y_max] = [%d,%d,%d,%d]", i, xmin, ymin, xmax, ymax);
    }
    for (int i = 0; i < num_of_detections; i++) {
        pthread_mutex_lock(&g_libc_mutex);
        float score = objectdetection_data.v_scores[i];
        pthread_mutex_unlock(&g_libc_mutex);

        PPL_DBG_PRINTF("scores[%d] = %f", i, score);
    }
    for (int i = 0; i < num_of_detections; i++) {
        pthread_mutex_lock(&g_libc_mutex);
        uint8_t class_indice = objectdetection_data.v_classes[i];
        pthread_mutex_unlock(&g_libc_mutex);

        PPL_DBG_PRINTF("class_indices[%d/%d] = %d", i, num_of_detections, class_indice);
    }

    return;
}

/* Function to serialize SSD MobilenetV1 output tensor data into Flatbuffers.
*/
static void createSSDOutputFlatbuffer(flatbuffers::FlatBufferBuilder* builder, const ObjectDetectionSsdData* ssdData) {
    std::vector<flatbuffers::Offset<SmartCamera::GeneralObject>> gdata_vector;

    PPL_DBG_PRINTF("createFlatbuffer");
    uint8_t numOfDetections = ssdData->numOfDetections;
    for (uint8_t i = 0; i < numOfDetections; i++) {
        PPL_DBG_PRINTF("left = %d, top = %d, right = %d, bottom = %d, class = %d, score = %f", ssdData->v_bbox[i].m_xmin, ssdData->v_bbox[i].m_ymin, ssdData->v_bbox[i].m_xmax, ssdData->v_bbox[i].m_ymax, ssdData->v_classes[i], ssdData->v_scores[i]);
        pthread_mutex_lock(&g_libc_mutex);
        auto bbox_data = SmartCamera::CreateBoundingBox2d(*builder, ssdData->v_bbox[i].m_xmin, \
            ssdData->v_bbox[i].m_ymin, \
            ssdData->v_bbox[i].m_xmax, \
            ssdData->v_bbox[i].m_ymax);
        pthread_mutex_unlock(&g_libc_mutex);

        pthread_mutex_lock(&g_libc_mutex);
        auto general_data = SmartCamera::CreateGeneralObject(*builder, ssdData->v_classes[i], SmartCamera::BoundingBox_BoundingBox2d, bbox_data.Union(), ssdData->v_scores[i]);
        pthread_mutex_unlock(&g_libc_mutex);

        pthread_mutex_lock(&g_libc_mutex);
        gdata_vector.push_back(general_data);
        pthread_mutex_unlock(&g_libc_mutex);
    }

    pthread_mutex_lock(&g_libc_mutex);
    auto v_bbox = builder->CreateVector(gdata_vector);
    pthread_mutex_unlock(&g_libc_mutex);

    pthread_mutex_lock(&g_libc_mutex);
    auto od_data = SmartCamera::CreateObjectDetectionData(*builder, v_bbox);
    pthread_mutex_unlock(&g_libc_mutex);

    pthread_mutex_lock(&g_libc_mutex);
    auto out_data = SmartCamera::CreateObjectDetectionTop(*builder, od_data);
    pthread_mutex_unlock(&g_libc_mutex);

    pthread_mutex_lock(&g_libc_mutex);
    builder->Finish(out_data);
    pthread_mutex_unlock(&g_libc_mutex);

    return;
}
