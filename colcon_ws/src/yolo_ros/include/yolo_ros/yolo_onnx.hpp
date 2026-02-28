#pragma once

#include <onnxruntime_cxx_api.h>
#include <opencv2/core.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace yolo_ros
{

struct Detection
{
    int class_id{0};
    float score{0.0F};
    cv::Rect box;
};

class YoloOnnx
{
public:
    YoloOnnx(const std::string& model_path, int input_width, int input_height,
             float conf_thres, float iou_thres, int ort_threads);

    std::vector<Detection> infer(const cv::Mat& bgr_image);

private:
    struct LetterboxInfo
    {
        float scale{1.0F};
        int pad_x{0};
        int pad_y{0};
        int original_width{0};
        int original_height{0};
    };

    LetterboxInfo preprocessToInput(const cv::Mat& bgr_image,
                                    std::vector<float>& chw_buffer) const;
    std::vector<Detection> decodeOutput(const float* output_data,
                                        const std::vector<int64_t>& shape,
                                        const LetterboxInfo& letterbox) const;

    static float maybeSigmoid(float value);

    int input_width_{640};
    int input_height_{640};
    float conf_thres_{0.25F};
    float iou_thres_{0.45F};

    Ort::Env env_;
    Ort::SessionOptions session_options_;
    Ort::Session session_{nullptr};
    Ort::MemoryInfo memory_info_{nullptr};

    std::string input_name_;
    std::vector<std::string> output_names_;
    std::vector<const char*> output_name_ptrs_;
    std::vector<int64_t> input_tensor_shape_;
    std::vector<float> input_buffer_;
};

}  // namespace yolo_ros
