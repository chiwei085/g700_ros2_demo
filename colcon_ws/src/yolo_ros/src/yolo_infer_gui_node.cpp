#include <builtin_interfaces/msg/time.hpp>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <rclcpp/executors/multi_threaded_executor.hpp>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "yolo_ros/coco_names.hpp"
#include "yolo_ros/yolo_onnx.hpp"

namespace yolo_ros
{

namespace
{

using ImageMsg = sensor_msgs::msg::Image;

constexpr char kWindowName[] = "YOLO";
constexpr double kDefaultFps = 2.0;
constexpr double kStatsIntervalSec = 5.0;

inline bool isQuitKey(const int key) {
    return key == 27 || key == 'q' || key == 'Q';
}

inline double safeFps(const double target_fps) {
    return target_fps > 0.0 ? target_fps : kDefaultFps;
}

inline double elapsedMs(const std::chrono::steady_clock::time_point& start,
                        const std::chrono::steady_clock::time_point& end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

struct RollingStats
{
    std::chrono::steady_clock::time_point stat_start{
        std::chrono::steady_clock::now()};
    double infer_ms_sum{0.0};
    int infer_count{0};

    void addSample(const double infer_ms) {
        infer_ms_sum += infer_ms;
        ++infer_count;
    }

    bool shouldReport(const std::chrono::steady_clock::time_point& now) const {
        const double elapsed_sec =
            std::chrono::duration<double>(now - stat_start).count();
        return elapsed_sec >= kStatsIntervalSec && infer_count > 0;
    }

    void reset(const std::chrono::steady_clock::time_point& now) {
        stat_start = now;
        infer_ms_sum = 0.0;
        infer_count = 0;
    }
};

}  // namespace

class YoloInferGuiNode : public rclcpp::Node
{
public:
    YoloInferGuiNode() : Node("yolo_infer") {
        config_.model_path = declare_parameter<std::string>(
            "model_path", "/ws/src/yolo_ros/model/model.onnx");
        config_.input_width = declare_parameter("input_width", 640);
        config_.input_height = declare_parameter("input_height", 640);
        config_.conf_thres = declare_parameter("conf_thres", 0.25);
        config_.iou_thres = declare_parameter("iou_thres", 0.45);
        config_.ort_threads = declare_parameter("ort_threads", 1);
        config_.target_fps = declare_parameter("target_fps", 2.0);
        config_.show_window = declare_parameter("show_window", true);
        config_.draw_labels = declare_parameter("draw_labels", true);
        config_.sample_policy =
            declare_parameter<std::string>("sample_policy", "latest");

        yolo_ = std::make_unique<YoloOnnx>(
            config_.model_path, config_.input_width, config_.input_height,
            static_cast<float>(config_.conf_thres),
            static_cast<float>(config_.iou_thres), config_.ort_threads);

        RCLCPP_INFO(get_logger(),
                    "yolo_infer config: model_path=%s target_fps=%.2f "
                    "ort_threads=%d sample_policy=%s "
                    "show_window=%s draw_labels=%s",
                    config_.model_path.c_str(), config_.target_fps,
                    config_.ort_threads, config_.sample_policy.c_str(),
                    config_.show_window ? "true" : "false",
                    config_.draw_labels ? "true" : "false");

        cb_group_ =
            this->create_callback_group(rclcpp::CallbackGroupType::Reentrant);
        sub_opts_.callback_group = cb_group_;
        image_sub_ = this->create_subscription<ImageMsg>(
            "/camera/camera/color/image_raw", rclcpp::SensorDataQoS(),
            [this](ImageMsg::SharedPtr msg) { imageCallback(std::move(msg)); },
            sub_opts_);

        infer_thread_ = std::thread(&YoloInferGuiNode::inferLoop, this);
    }

    ~YoloInferGuiNode() override {
        stopAndJoinInferThread();
        if (config_.show_window) {
            cv::destroyWindow(kWindowName);
        }
    }

private:
    struct Config
    {
        std::string model_path;
        int input_width{640};
        int input_height{640};
        double conf_thres{0.25};
        double iou_thres{0.45};
        int ort_threads{1};
        double target_fps{2.0};
        bool show_window{true};
        bool draw_labels{true};
        std::string sample_policy{"latest"};
    };

    void stopAndJoinInferThread() {
        stop_.store(true);
        if (infer_thread_.joinable()) {
            infer_thread_.join();
        }
    }

    void imageCallback(const ImageMsg::SharedPtr msg) {
        cv_bridge::CvImagePtr cv_ptr;
        try {
            cv_ptr = cv_bridge::toCvCopy(msg, "bgr8");
        }
        catch (const cv_bridge::Exception& ex) {
            RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000,
                                 "cv_bridge conversion failed: %s", ex.what());
            return;
        }

        std::lock_guard<std::mutex> lock(frame_mutex_);
        latest_frame_ = cv_ptr->image.clone();
        latest_stamp_ = msg->header.stamp;
        has_frame_ = true;
    }

    void inferLoop() {
        if (config_.show_window) {
            cv::namedWindow(kWindowName, cv::WINDOW_NORMAL);
        }

        const auto target_period =
            std::chrono::duration<double>(1.0 / safeFps(config_.target_fps));
        RollingStats stats;

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

                stats.addSample(elapsedMs(infer_begin, infer_end));
                drawDetections(frame, detections);

                if (config_.show_window) {
                    cv::imshow(kWindowName, frame);
                    const int key = cv::waitKey(1);
                    if (isQuitKey(key)) {
                        stop_.store(true);
                        rclcpp::shutdown();
                        break;
                    }
                }
            }

            const auto now = std::chrono::steady_clock::now();
            if (stats.shouldReport(now)) {
                const double elapsed_sec =
                    std::chrono::duration<double>(now - stats.stat_start)
                        .count();
                const double avg_ms =
                    stats.infer_ms_sum / static_cast<double>(stats.infer_count);
                const double effective_fps =
                    static_cast<double>(stats.infer_count) / elapsed_sec;
                RCLCPP_INFO(get_logger(),
                            "avg infer: %.2f ms, effective infer fps: %.2f",
                            avg_ms, effective_fps);
                stats.reset(now);
            }

            const auto loop_elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - loop_begin);
            if (loop_elapsed < target_period) {
                std::this_thread::sleep_for(target_period - loop_elapsed);
            }
        }
    }

    void drawDetections(cv::Mat& frame,
                        const std::vector<Detection>& detections) const {
        for (const auto& det : detections) {
            cv::rectangle(frame, det.box, cv::Scalar(0, 255, 0), 2);
            if (!config_.draw_labels) {
                continue;
            }

            const std::string class_name =
                det.class_id >= 0 && static_cast<size_t>(det.class_id) <
                                         kCocoClassNames.size()
                    ? std::string(
                          kCocoClassNames[static_cast<size_t>(det.class_id)])
                    : std::string("cls_") + std::to_string(det.class_id);
            const std::string label =
                class_name + " " + cv::format("%.2f", det.score);

            int baseline = 0;
            const cv::Size text_size = cv::getTextSize(
                label, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseline);
            const int top = std::max(det.box.y, text_size.height + 4);
            cv::rectangle(
                frame, cv::Point(det.box.x, top - text_size.height - 4),
                cv::Point(det.box.x + text_size.width + 4, top + baseline - 4),
                cv::Scalar(0, 255, 0), cv::FILLED);
            cv::putText(frame, label, cv::Point(det.box.x + 2, top - 4),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0), 1);
        }
    }

private:
    std::mutex frame_mutex_;
    cv::Mat latest_frame_;
    builtin_interfaces::msg::Time latest_stamp_;
    bool has_frame_{false};
    std::atomic<bool> stop_{false};

    Config config_;
    std::unique_ptr<YoloOnnx> yolo_;
    std::thread infer_thread_;

    rclcpp::CallbackGroup::SharedPtr cb_group_;
    rclcpp::SubscriptionOptions sub_opts_;
    rclcpp::Subscription<ImageMsg>::SharedPtr image_sub_;
};

}  // namespace yolo_ros

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<yolo_ros::YoloInferGuiNode>();
    rclcpp::executors::MultiThreadedExecutor exec(rclcpp::ExecutorOptions(), 2);
    exec.add_node(node);
    exec.spin();
    exec.remove_node(node);
    rclcpp::shutdown();
    return 0;
}
