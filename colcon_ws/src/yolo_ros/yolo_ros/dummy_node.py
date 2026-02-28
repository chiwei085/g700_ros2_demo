import rclpy
from rclpy.node import Node


class DummyNode(Node):
    def __init__(self) -> None:
        super().__init__("yolo_dummy")
        self._timer = self.create_timer(2.0, self._on_timer)

    def _on_timer(self) -> None:
        self.get_logger().info("yolo_ros dummy alive")


def main(args=None) -> None:
    rclpy.init(args=args)
    node = DummyNode()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        rclpy.shutdown()
