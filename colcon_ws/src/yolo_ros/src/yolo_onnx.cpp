#include "yolo_ros/yolo_onnx.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

namespace yolo_ros
{

namespace
{

constexpr int kNumClasses = 80;
constexpr int kStrideNoObj = 84;  // 4 box + 80 classes
constexpr int kStrideObj = 85;    // 4 box + 1 obj + 80 classes

inline int clampInt(const int value, const int low, const int high)
{
  return std::max(low, std::min(value, high));
}

}  // namespace

YoloOnnx::YoloOnnx(
  const std::string & model_path, const int input_width, const int input_height,
  const float conf_thres, const float iou_thres, int ort_threads)
: input_width_(input_width),
  input_height_(input_height),
  conf_thres_(conf_thres),
  iou_thres_(iou_thres),
  env_(ORT_LOGGING_LEVEL_WARNING, "yolo_ros"),
  memory_info_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
{
  if (ort_threads < 1) {
    ort_threads = 1;
  }

  session_options_.SetIntraOpNumThreads(ort_threads);
  session_options_.SetInterOpNumThreads(1);
  session_options_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
  session_options_.EnableCpuMemArena();
  session_options_.EnableMemPattern();

  session_ = Ort::Session(env_, model_path.c_str(), session_options_);
  input_tensor_shape_ = {1, 3, input_height_, input_width_};
  input_buffer_.resize(static_cast<size_t>(3 * input_width_ * input_height_));

  Ort::AllocatorWithDefaultOptions allocator;
  {
    const auto input_name_alloc = session_.GetInputNameAllocated(0, allocator);
    input_name_ = input_name_alloc.get();
  }

  const size_t output_count = session_.GetOutputCount();
  output_names_.reserve(output_count);
  output_name_ptrs_.reserve(output_count);
  for (size_t i = 0; i < output_count; ++i) {
    const auto output_name_alloc = session_.GetOutputNameAllocated(i, allocator);
    output_names_.emplace_back(output_name_alloc.get());
  }
  for (auto & output_name : output_names_) {
    output_name_ptrs_.push_back(output_name.c_str());
  }
}

std::vector<Detection> YoloOnnx::infer(const cv::Mat & bgr_image)
{
  if (bgr_image.empty()) {
    return {};
  }

  const LetterboxInfo letterbox = preprocessToInput(bgr_image, input_buffer_);

  Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
    memory_info_, input_buffer_.data(), input_buffer_.size(), input_tensor_shape_.data(),
    input_tensor_shape_.size());

  const char * input_names[] = {input_name_.c_str()};
  auto output_tensors = session_.Run(
    Ort::RunOptions{nullptr}, input_names, &input_tensor, 1, output_name_ptrs_.data(),
    output_name_ptrs_.size());

  if (output_tensors.empty()) {
    return {};
  }

  const auto & output = output_tensors.front();
  const auto shape = output.GetTensorTypeAndShapeInfo().GetShape();
  const float * output_data = output.GetTensorData<float>();
  return decodeOutput(output_data, shape, letterbox);
}

YoloOnnx::LetterboxInfo YoloOnnx::preprocessToInput(
  const cv::Mat & bgr_image, std::vector<float> & chw_buffer) const
{
  const int original_width = bgr_image.cols;
  const int original_height = bgr_image.rows;
  const float scale = std::min(
    static_cast<float>(input_width_) / static_cast<float>(original_width),
    static_cast<float>(input_height_) / static_cast<float>(original_height));

  const int resized_width = static_cast<int>(std::round(original_width * scale));
  const int resized_height = static_cast<int>(std::round(original_height * scale));
  const int pad_x = (input_width_ - resized_width) / 2;
  const int pad_y = (input_height_ - resized_height) / 2;

  cv::Mat resized;
  cv::resize(bgr_image, resized, cv::Size(resized_width, resized_height), 0.0, 0.0, cv::INTER_LINEAR);

  cv::Mat letterboxed(input_height_, input_width_, CV_8UC3, cv::Scalar(114, 114, 114));
  resized.copyTo(letterboxed(cv::Rect(pad_x, pad_y, resized_width, resized_height)));

  cv::Mat rgb;
  cv::cvtColor(letterboxed, rgb, cv::COLOR_BGR2RGB);

  const int area = input_width_ * input_height_;
  const float scale_norm = 1.0F / 255.0F;
  for (int y = 0; y < input_height_; ++y) {
    const cv::Vec3b * row_ptr = rgb.ptr<cv::Vec3b>(y);
    const int row_offset = y * input_width_;
    for (int x = 0; x < input_width_; ++x) {
      const cv::Vec3b & pixel = row_ptr[x];
      const int index = row_offset + x;
      chw_buffer[index] = static_cast<float>(pixel[0]) * scale_norm;
      chw_buffer[area + index] = static_cast<float>(pixel[1]) * scale_norm;
      chw_buffer[2 * area + index] = static_cast<float>(pixel[2]) * scale_norm;
    }
  }

  LetterboxInfo info;
  info.scale = scale;
  info.pad_x = pad_x;
  info.pad_y = pad_y;
  info.original_width = original_width;
  info.original_height = original_height;
  return info;
}

std::vector<Detection> YoloOnnx::decodeOutput(
  const float * output_data, const std::vector<int64_t> & shape, const LetterboxInfo & letterbox) const
{
  if (shape.size() < 2) {
    throw std::runtime_error("YOLO output rank is too small.");
  }

  int64_t num_preds = 0;
  int64_t stride = 0;
  bool channels_first = false;

  if (shape.size() == 3) {
    // [1,84,8400] or [1,8400,84]
    if (shape[1] <= 256 && shape[2] > 256) {
      channels_first = true;
      stride = shape[1];
      num_preds = shape[2];
    } else {
      channels_first = false;
      num_preds = shape[1];
      stride = shape[2];
    }
  } else {
    // fallback for [8400,84] / [8400,85]
    num_preds = shape[0];
    stride = shape[1];
  }

  if (stride != kStrideNoObj && stride != kStrideObj) {
    throw std::runtime_error("Unsupported YOLO output stride, expected 84 or 85.");
  }

  const bool has_objectness = stride == kStrideObj;
  const int class_offset = has_objectness ? 5 : 4;
  const int class_count = kNumClasses;

  auto valueAt = [&](const int64_t pred_index, const int channel) -> float {
      if (channels_first) {
        return output_data[static_cast<size_t>(channel) * static_cast<size_t>(num_preds) + pred_index];
      }
      return output_data[pred_index * static_cast<size_t>(stride) + static_cast<size_t>(channel)];
    };

  std::vector<cv::Rect> boxes;
  std::vector<float> scores;
  std::vector<int> class_ids;
  boxes.reserve(static_cast<size_t>(num_preds));
  scores.reserve(static_cast<size_t>(num_preds));
  class_ids.reserve(static_cast<size_t>(num_preds));

  for (int64_t i = 0; i < num_preds; ++i) {
    const float cx = valueAt(i, 0);
    const float cy = valueAt(i, 1);
    const float w = valueAt(i, 2);
    const float h = valueAt(i, 3);

    float objectness = 1.0F;
    if (has_objectness) {
      objectness = maybeSigmoid(valueAt(i, 4));
      if (objectness < conf_thres_) {
        continue;
      }
    }

    float best_class_score = -std::numeric_limits<float>::infinity();
    int best_class_id = -1;
    for (int c = 0; c < class_count; ++c) {
      const float cls_score = valueAt(i, class_offset + c);
      if (cls_score > best_class_score) {
        best_class_score = cls_score;
        best_class_id = c;
      }
    }

    if (best_class_id < 0) {
      continue;
    }

    best_class_score = maybeSigmoid(best_class_score);
    const float score = objectness * best_class_score;
    if (score < conf_thres_) {
      continue;
    }

    const float x1 = (cx - 0.5F * w - static_cast<float>(letterbox.pad_x)) / letterbox.scale;
    const float y1 = (cy - 0.5F * h - static_cast<float>(letterbox.pad_y)) / letterbox.scale;
    const float x2 = (cx + 0.5F * w - static_cast<float>(letterbox.pad_x)) / letterbox.scale;
    const float y2 = (cy + 0.5F * h - static_cast<float>(letterbox.pad_y)) / letterbox.scale;

    const int left = clampInt(static_cast<int>(std::floor(x1)), 0, letterbox.original_width - 1);
    const int top = clampInt(static_cast<int>(std::floor(y1)), 0, letterbox.original_height - 1);
    const int right = clampInt(static_cast<int>(std::ceil(x2)), 0, letterbox.original_width - 1);
    const int bottom = clampInt(static_cast<int>(std::ceil(y2)), 0, letterbox.original_height - 1);

    if (right <= left || bottom <= top) {
      continue;
    }

    boxes.emplace_back(left, top, right - left, bottom - top);
    scores.push_back(score);
    class_ids.push_back(best_class_id);
  }

  std::vector<int> kept_indices;
  cv::dnn::NMSBoxes(boxes, scores, conf_thres_, iou_thres_, kept_indices);

  std::vector<Detection> detections;
  detections.reserve(kept_indices.size());
  for (const int idx : kept_indices) {
    Detection detection;
    detection.class_id = class_ids[static_cast<size_t>(idx)];
    detection.score = scores[static_cast<size_t>(idx)];
    detection.box = boxes[static_cast<size_t>(idx)];
    detections.push_back(detection);
  }
  return detections;
}

float YoloOnnx::maybeSigmoid(const float value)
{
  if (value >= 0.0F && value <= 1.0F) {
    return value;
  }
  return 1.0F / (1.0F + std::exp(-value));
}

}  // namespace yolo_ros
