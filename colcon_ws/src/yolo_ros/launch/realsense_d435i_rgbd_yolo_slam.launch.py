import os

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, IncludeLaunchDescription
from launch.conditions import IfCondition
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def generate_launch_description() -> LaunchDescription:
    orb_share = FindPackageShare("orbslam3_ros2")
    passthrough_args = [
        ("voc_file", PathJoinSubstitution([orb_share, "Vocabulary", "ORBvoc.txt"])),
        ("settings_file", PathJoinSubstitution([orb_share, "RGB-D", "RealSense_D435i_save.yaml"])),
        ("map_frame_id", "map"),
        ("cam_frame_id", "camera_link"),
        ("enable_pangolin", "true"),
        ("viewer_backend", "auto"),
        ("realsense_with_sudo", "true"),
        ("start_realsense", "false"),
        ("wait_for_topics", "true"),
        ("topics_timeout_sec", "20.0"),
        ("orb_start_delay_sec", "5.0"),
        ("orb_shutdown_after_sec", "0.0"),
        ("check_outputs", "false"),
        ("output_check_timeout_sec", "8.0"),
        ("asan_options", ""),
        ("lsan_options", ""),
        ("debug_mode", "false"),
        ("orb_log_file", "/tmp/orbslam3_orb.log"),
        ("realsense_log_file", "/tmp/orbslam3_realsense.log"),
        ("dump_asan_first_error", "true"),
    ]

    declared_args = [
        DeclareLaunchArgument("enable_yolo_infer", default_value="true"),
        DeclareLaunchArgument("yolo_model_path", default_value="/ws/src/yolo_ros/model/model.onnx"),
        DeclareLaunchArgument("yolo_target_fps", default_value="2.0"),
        DeclareLaunchArgument("yolo_ort_threads", default_value="1"),
        DeclareLaunchArgument("yolo_show_window", default_value="true"),
        DeclareLaunchArgument("yolo_conf_thres", default_value="0.25"),
        DeclareLaunchArgument("yolo_iou_thres", default_value="0.45"),
    ]
    for name, default in passthrough_args:
        declared_args.append(DeclareLaunchArgument(name, default_value=default))

    orb_share_path = get_package_share_directory("orbslam3_ros2")
    orb_launch_file = "realsense_d435i_rgbd_wsl_save.launch.py"
    orb_launch_path = os.path.join(orb_share_path, "launch", orb_launch_file)
    if not os.path.exists(orb_launch_path):
        orb_launch_path = os.path.join(orb_share_path, orb_launch_file)

    orbslam_launch = IncludeLaunchDescription(
        PythonLaunchDescriptionSource(
            orb_launch_path
        ),
        launch_arguments={name: LaunchConfiguration(name) for name, _ in passthrough_args}.items(),
    )

    yolo_node = Node(
        package="yolo_ros",
        executable="yolo_infer_gui_node",
        name="yolo_infer",
        output="screen",
        condition=IfCondition(LaunchConfiguration("enable_yolo_infer")),
        parameters=[
            {
                "model_path": LaunchConfiguration("yolo_model_path"),
                "target_fps": LaunchConfiguration("yolo_target_fps"),
                "ort_threads": LaunchConfiguration("yolo_ort_threads"),
                "show_window": LaunchConfiguration("yolo_show_window"),
                "conf_thres": LaunchConfiguration("yolo_conf_thres"),
                "iou_thres": LaunchConfiguration("yolo_iou_thres"),
            }
        ],
    )

    return LaunchDescription(declared_args + [orbslam_launch, yolo_node])
