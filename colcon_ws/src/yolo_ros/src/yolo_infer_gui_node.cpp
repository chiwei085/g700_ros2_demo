#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <builtin_interfaces/msg/time.hpp>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp/executors/multi_threaded_executor.hpp>
#include <sensor_msgs/msg/image.hpp>

#include "yolo_ros/coco_names.hpp"
#include "yolo_ros/yolo_onnx.hpp"

namespace yolo_ros
{

class YoloInferGuiNode : public rclcpp::Node
{
public:
  YoloInferGuiNode()
  : Node("yolo_infer")
  {
    model_path_ = this->declare_parameter<std::string>("model_path", "/ws/src/yolo_ros/model/model.onnx");
    input_width_ = this->declare_parameter<int>("input_width", 640);
    input_height_ = this->declare_parameter<int>("input_height", 640);
    conf_thres_ = this->declare_parameter<double>("conf_thres", 0.25);
    iou_thres_ = this->declare_parameter<double>("iou_thres", 0.45);
    ort_threads_ = this->declare_parameter<int>("ort_threads", 1);
    target_fps_ = this->declare_parameter<double>("target_fps", 2.0);
    show_window_ = this->declare_parameter<bool>("show_window", true);
    draw_labels_ = this->declare_parameter<bool>("draw_labels", true);
    sample_policy_ = this->declare_parameter<std::string>("sample_policy", "latest");

    yolo_ = std::make_unique<YoloOnnx>(
      model_path_, input_width_, input_height_, static_cast<float>(conf_thres_),
      static_cast<float>(iou_thres_), ort_threads_);

    RCLCPP_INFO(
      get_logger(),
      "yolo_infer config: model_path=%s target_fps=%.2f ort_threads=%d sample_policy=%s "
      "show_window=%s draw_labels=%s",
      model_path_.c_str(), target_fps_, ort_threads_, sample_policy_.c_str(), show_window_ ? "true" : "false",
      draw_labels_ ? "true" : "false");

    image_callback_group_ = this->create_callback_group(rclcpp::CallbackGroupType::Reentrant);
    rclcpp::SubscriptionOptions image_sub_opts;
    image_sub_opts.callback_group = image_callback_group_;
    image_sub_ = this->create_subscription<sensor_msgs::msg::Image>(
      "/camera/camera/color/image_raw",
      rclcpp::SensorDataQoS(),
      std::bind(&YoloInferGuiNode::imageCallback, this, std::placeholders::_1),
      image_sub_opts);

    infer_thread_ = std::thread(&YoloInferGuiNode::inferLoop, this);
  }

  ~YoloInferGuiNode() override
  {
    stop_.store(true);
    if (infer_thread_.joinable()) {
      infer_thread_.join();
    }
    if (show_window_) {
      cv::destroyWindow("YOLO");
    }
  }

private:
  void imageCallback(const sensor_msgs::msg::Image::SharedPtr msg)
  {
    cv_bridge::CvImagePtr cv_ptr;
    try {
      cv_ptr = cv_bridge::toCvCopy(msg, "bgr8");
    } catch (const cv_bridge::Exception & ex) {
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 5000, "cv_bridge conversion failed: %s", ex.what());
      return;
    }

    std::lock_guard<std::mutex> lock(frame_mutex_);
    latest_frame_ = cv_ptr->image.clone();
    latest_stamp_ = msg->header.stamp;
    has_frame_ = true;
  }

  void inferLoop()
  {
    if (show_window_) {
      cv::namedWindow("YOLO", cv::WINDOW_NORMAL);
    }

    const double safe_fps = target_fps_ > 0.0 ? target_fps_ : 2.0;
    const auto target_period = std::chrono::duration<double>(1.0 / safe_fps);

    auto stat_start = std::chrono::steady_clock::now();
    double infer_ms_sum = 0.0;
    int infer_count = 0;

    while (rclcpp::ok() && !stop_.load()) {
      const auto loop_begin = std::chrono::steady_clock::now();

      cv::Mat frame;
      {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (has_frame_ && !latest_frame_.empty()) {
          frame = latest_frame_;
        }
      }

      if (!frame.empty()) {
        const auto infer_begin = std::chrono::steady_clock::now();
        const std::vector<Detection> detections = yolo_->infer(frame);
        const auto infer_end = std::chrono::steady_clock::now();

        const double infer_ms =
          std::chrono::duration<double, std::milli>(infer_end - infer_begin).count();
        infer_ms_sum += infer_ms;
        ++infer_count;

        drawDetections(frame, detections);

        if (show_window_) {
          cv::imshow("YOLO", frame);
          const int key = cv::waitKey(1);
          if (key == 27 || key == 'q' || key == 'Q') {
            stop_.store(true);
            rclcpp::shutdown();
            break;
          }
        }
      }

      const auto now = std::chrono::steady_clock::now();
      const auto stat_elapsed = std::chrono::duration<double>(now - stat_start).count();
      if (stat_elapsed >= 5.0 && infer_count > 0) {
        const double avg_ms = infer_ms_sum / static_cast<double>(infer_count);
        const double effective_fps = static_cast<double>(infer_count) / stat_elapsed;
        RCLCPP_INFO(
          get_logger(), "avg infer: %.2f ms, effective infer fps: %.2f", avg_ms, effective_fps);
        stat_start = now;
        infer_ms_sum = 0.0;
        infer_count = 0;
      }

      const auto loop_elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - loop_begin);
      if (loop_elapsed < target_period) {
        std::this_thread::sleep_for(target_period - loop_elapsed);
      }
    }
  }

  void drawDetections(cv::Mat & frame, const std::vector<Detection> & detections) const
  {
    for (const auto & det : detections) {
      cv::rectangle(frame, det.box, cv::Scalar(0, 255, 0), 2);
      if (!draw_labels_) {
        continue;
      }
      const std::string class_name =
        det.class_id >= 0 && static_cast<size_t>(det.class_id) < kCocoClassNames.size()
        ? std::string(kCocoClassNames[static_cast<size_t>(det.class_id)])
        : std::string("cls_") + std::to_string(det.class_id);
      const std::string label = class_name + " " + cv::format("%.2f", det.score);

      int baseline = 0;
      const cv::Size text_size =
        cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseline);
      const int top = std::max(det.box.y, text_size.height + 4);
      cv::rectangle(
        frame,
        cv::Point(det.box.x, top - text_size.height - 4),
        cv::Point(det.box.x + text_size.width + 4, top + baseline - 4),
        cv::Scalar(0, 255, 0),
        cv::FILLED);
      cv::putText(
        frame, label, cv::Point(det.box.x + 2, top - 4), cv::FONT_HERSHEY_SIMPLEX, 0.5,
        cv::Scalar(0, 0, 0), 1);
    }
  }

  std::mutex frame_mutex_;
  cv::Mat latest_frame_;
  builtin_interfaces::msg::Time latest_stamp_;
  bool has_frame_{false};
  std::atomic<bool> stop_{false};

  std::unique_ptr<YoloOnnx> yolo_;
  std::thread infer_thread_;
  rclcpp::CallbackGroup::SharedPtr image_callback_group_;
  rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr image_sub_;

  std::string model_path_;
  int input_width_{640};
  int input_height_{640};
  double conf_thres_{0.25};
  double iou_thres_{0.45};
  int ort_threads_{1};
  double target_fps_{2.0};
  bool show_window_{true};
  bool draw_labels_{true};
  std::string sample_policy_{"latest"};
};

}  // namespace yolo_ros

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<yolo_ros::YoloInferGuiNode>();
  rclcpp::executors::MultiThreadedExecutor exec(rclcpp::ExecutorOptions(), 2);
  exec.add_node(node);
  exec.spin();
  exec.remove_node(node);
  rclcpp::shutdown();
  return 0;
}
