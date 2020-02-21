/**
 * @brief Altitude plugin
 * @file altitude.cpp
 * @author Andreas Antener <andreas@uaventure.com>
 *
 * @addtogroup plugin
 * @{
 */
/*
 * Copyright 2015 Andreas Antener <andreas@uaventure.com>.
 *
 * This file is part of the mavros package and subject to the license terms
 * in the top-level LICENSE file of the mavros repository.
 * https://github.com/mavlink/mavros/tree/master/LICENSE.md
 */

#include <mavros/mavros_plugin.h>

#include <mavros_msgs/msg/altitude.hpp>

namespace mavros {
namespace std_plugins {
/**
 * @brief Altitude plugin.
 */
class AltitudePlugin : public plugin::PluginBase {
public:
	AltitudePlugin() : PluginBase(),
		nh(rclcpp::Node::make_shared("altitude", "mavros"))
	{ }

	/**
	 * Plugin initializer. Constructor should not do this.
	 */
	void initialize(UAS &uas_)
	{
		PluginBase::initialize(uas_);

		frame_id = nh->declare_parameter<std::string>("frame_id", "map");
		altitude_pub = nh->create_publisher<mavros_msgs::msg::Altitude>("altitude", 10);
	}

	Subscriptions get_subscriptions()
	{
		return {
			make_handler(&AltitudePlugin::handle_altitude),
		};
	}

	rclcpp::Node::SharedPtr get_ros_node() override {
		return nh;
	}

private:
	rclcpp::Node::SharedPtr nh;
	std::string frame_id;

	rclcpp::Publisher<mavros_msgs::msg::Altitude>::SharedPtr altitude_pub;

	void handle_altitude(const mavlink::mavlink_message_t *msg, mavlink::common::msg::ALTITUDE &altitude)
	{
		auto ros_msg = std::make_shared<mavros_msgs::msg::Altitude>();
		ros_msg->header = m_uas->synchronized_header(frame_id, altitude.time_usec);

		ros_msg->monotonic = altitude.altitude_monotonic;
		ros_msg->amsl = altitude.altitude_amsl;
		ros_msg->local = altitude.altitude_local;
		ros_msg->relative = altitude.altitude_relative;
		ros_msg->terrain = altitude.altitude_terrain;
		ros_msg->bottom_clearance = altitude.bottom_clearance;

		altitude_pub->publish(*ros_msg);
	}
};
}	// namespace std_plugins
}	// namespace mavros

#include <pluginlib/class_list_macros.hpp>
PLUGINLIB_EXPORT_CLASS(mavros::std_plugins::AltitudePlugin, mavros::plugin::PluginBase)
