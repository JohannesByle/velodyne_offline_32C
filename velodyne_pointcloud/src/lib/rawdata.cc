/*
 *  Copyright (C) 2007 Austin Robot Technology, Patrick Beeson
 *  Copyright (C) 2009, 2010, 2012 Austin Robot Technology, Jack O'Quin
 *
 *  License: Modified BSD Software License Agreement
 *
 *  $Id$
 */

/**
 *  @file
 *
 *  Velodyne 3D LIDAR data accessor class implementation.
 *
 *  Class for unpacking raw Velodyne LIDAR packets into useful
 *  formats.
 *
 *  Derived classes accept raw Velodyne data for either single packets
 *  or entire rotations, and provide it in various formats for either
 *  on-line or off-line processing.
 *
 *  @author Patrick Beeson
 *  @author Jack O'Quin
 *
 *  HDL-64E S2 calibration support provided by Nick Hillier
 */

#include <fstream>

#include <ros/ros.h>
#include <ros/package.h>
#include <angles/angles.h>

#include <velodyne_pointcloud/rawdata.h>

namespace velodyne_rawdata
{
  ////////////////////////////////////////////////////////////////////////
  //
  // RawData base class implementation
  //
  ////////////////////////////////////////////////////////////////////////

  RawData::RawData() {}
  
  /** Uppdate parameters: conversions and update */
  void RawData::setParameters(double min_range,
                              double max_range,
                              double view_center,
                              double left_most_angle,
                              double right_most_angle)
  {
  config_.min_range = min_range;
  config_.max_range = max_range;

  //putting left_most_angle and right_most_angle in the velodyne reference (rad)
  config_.tmp_min_angle = view_center + left_most_angle;
  config_.tmp_max_angle = view_center - right_most_angle;
  
  //computing positive modulo to keep theses angles into [0;2*M_PI]
  config_.tmp_min_angle = fmod(fmod(config_.tmp_min_angle,2*M_PI) + 2*M_PI,2*M_PI);
  config_.tmp_max_angle = fmod(fmod(config_.tmp_max_angle,2*M_PI) + 2*M_PI,2*M_PI);
  
  //converting into the hardware velodyne ref (negative yaml and degrees)
  //adding 0.5 perfomrs a centered double to int conversion 
  config_.min_angle = 100 * (2*M_PI - config_.tmp_min_angle) * 180 / M_PI + 0.5;
  config_.max_angle = 100 * (2*M_PI - config_.tmp_max_angle) * 180 / M_PI + 0.5;
  if (config_.min_angle == config_.max_angle)
  {
    config_.min_angle = 0;
    config_.max_angle = 36000;
  }
  
  ROS_INFO("M_PI value: %f", M_PI);
  ROS_INFO("2M_PI value: %f", 2.0*M_PI);
  ROS_INFO("min_angle into setParameters: %i",config_.min_angle);
  ROS_INFO("max_angle into setParameters: %i",config_.max_angle);
  }

  /** Set up for on-line operation. */
  int RawData::setup(ros::NodeHandle private_nh)
  {
    /*private_nh.param("max_range", config_.max_range,
                     (double) velodyne_rawdata::DISTANCE_MAX);
    private_nh.param("min_range", config_.min_range, 2.0);
    private_nh.param("min_angle", config_.min_angle, 0.0);
    private_nh.param("max_angle", config_.max_angle, 2.0 * M_PI);
    //converting min_angle and max_angles in degrees
    config_.min_angle = (2*M_PI - config_.max_angle) * 180 / M_PI;
    config_.max_angle = (2*M_PI - config_.min_angle) * 180 / M_PI;
    
    ROS_INFO_STREAM("data ranges to publish: ["
                    << config_.min_range << ", "
                    << config_.max_range << "]");
    ROS_INFO_STREAM("data angles to publish: ["
                    << config_.min_angle << ", "
                    << config_.max_angle << "]");*/

    // get path to angles.config file for this device
    if (!private_nh.getParam("calibration", config_.calibrationFile))
      {
        ROS_ERROR_STREAM("No calibration angles specified! Using test values!");

        // have to use something: grab unit test version as a default
        std::string pkgPath = ros::package::getPath("velodyne_pointcloud");
        config_.calibrationFile = pkgPath + "/params/64e_utexas.yaml";
      }

    ROS_INFO_STREAM("correction angles: " << config_.calibrationFile);

    calibration_.read(config_.calibrationFile);
    if (!calibration_.initialized) {
      ROS_ERROR_STREAM("Unable to open calibration file: " << 
          config_.calibrationFile);
      return -1;
    }

    // Set up cached values for sin and cos of all the possible headings
    for (uint16_t rot_index = 0; rot_index < ROTATION_MAX_UNITS; ++rot_index) {
      float rotation = angles::from_degrees(ROTATION_RESOLUTION * rot_index);
      cos_rot_table_[rot_index] = cosf(rotation);
      sin_rot_table_[rot_index] = sinf(rotation);
    }
   return 0;
  }

  /** @brief convert raw packet to point cloud
   *
   *  @param pkt raw packet to unpack
   *  @param pc shared pointer to point cloud (points are appended)
   */
  void RawData::unpack(const velodyne_msgs::VelodynePacket &pkt,
                       VPointCloud &pc)
  {
    ROS_DEBUG_STREAM("Received packet, time: " << pkt.stamp);

    const raw_packet_t *raw = (const raw_packet_t *) &pkt.data[0];

    for (int i = 0; i < BLOCKS_PER_PACKET; i++) {

      // upper bank lasers are numbered [0..31]
      // NOTE: this is a change from the old velodyne_common implementation
      int bank_origin = 0;
      if (raw->blocks[i].header == LOWER_BANK) {
        // lower bank lasers are [32..63]
        bank_origin = 32;
      }

      for (int j = 0, k = 0; j < SCANS_PER_BLOCK; j++, k += RAW_SCAN_SIZE) {
        
        float x, y, z;
        float intensity;
        uint8_t laser_number;       ///< hardware laser number

        laser_number = j + bank_origin;
        velodyne_pointcloud::LaserCorrection &corrections = 
          calibration_.laser_corrections[laser_number];

        /** Position Calculation */

        union two_bytes tmp;
        tmp.bytes[0] = raw->blocks[i].data[k];
        tmp.bytes[1] = raw->blocks[i].data[k+1];
        /*condition added to avoid calculating points which are not
          in the interesting defined area (min_angle < area < max_angle)*/
        if ((raw->blocks[i].rotation >= config_.min_angle 
             && raw->blocks[i].rotation <= config_.max_angle 
             && config_.min_angle < config_.max_angle)
             ||(config_.min_angle > config_.max_angle 
             && (raw->blocks[i].rotation <= config_.max_angle 
             || raw->blocks[i].rotation >= config_.min_angle))){
          float distance = tmp.uint * DISTANCE_RESOLUTION;
          distance += corrections.dist_correction;
  
          float cos_vert_angle = corrections.cos_vert_correction;
          float sin_vert_angle = corrections.sin_vert_correction;
          float cos_rot_correction = corrections.cos_rot_correction;
          float sin_rot_correction = corrections.sin_rot_correction;
  
          // cos(a-b) = cos(a)*cos(b) + sin(a)*sin(b)
          // sin(a-b) = sin(a)*cos(b) - cos(a)*sin(b)
          float cos_rot_angle = 
            cos_rot_table_[raw->blocks[i].rotation] * cos_rot_correction + 
            sin_rot_table_[raw->blocks[i].rotation] * sin_rot_correction;
          float sin_rot_angle = 
            sin_rot_table_[raw->blocks[i].rotation] * cos_rot_correction - 
            cos_rot_table_[raw->blocks[i].rotation] * sin_rot_correction;
  
          float horiz_offset = corrections.horiz_offset_correction;
          float vert_offset = corrections.vert_offset_correction;
  
          // Compute the distance in the xy plane (w/o accounting for rotation)
          float xy_distance = distance * cos_vert_angle;
  
          // Calculate temporal X, use absolute value.
          float xx = xy_distance * sin_rot_angle - horiz_offset * cos_rot_angle;
          // Calculate temporal Y, use absolute value
          float yy = xy_distance * cos_rot_angle + horiz_offset * sin_rot_angle;
          if (xx < 0) xx=-xx;
          if (yy < 0) yy=-yy;
    
          // Get 2points calibration values,Linear interpolation to get distance
          // correction for X and Y, that means distance correction use
          // different value at different distance
          float distance_corr_x = 0;
          float distance_corr_y = 0;
          if (corrections.two_pt_correction_available) {
            distance_corr_x = 
              (corrections.dist_correction - corrections.dist_correction_x)
                * (xx - 2.4) / (25.04 - 2.4) 
              + corrections.dist_correction_x;
            distance_corr_y = 
              (corrections.dist_correction - corrections.dist_correction_y)
                * (yy - 1.93) / (25.04 - 1.93)
              + corrections.dist_correction_y;
          }
  
          float distance_x = distance + distance_corr_x;
          xy_distance = distance_x * cos_vert_angle;
          x = xy_distance * sin_rot_angle + horiz_offset * cos_rot_angle;
  
          float distance_y = distance + distance_corr_y;
          xy_distance = distance_y * cos_vert_angle;
          y = xy_distance * cos_rot_angle + horiz_offset * sin_rot_angle;
  
          z = distance * sin_vert_angle + vert_offset;
  
          /** Use standard ROS coordinate system (right-hand rule) */
          float x_coord = y;
          float y_coord = -x;
          float z_coord = z;
  
          /** Intensity Calculation */
  
          float min_intensity = corrections.min_intensity;
          float max_intensity = corrections.max_intensity;
  
          intensity = raw->blocks[i].data[k+2];
  
          float focal_offset = 256 
                             * (1 - corrections.focal_distance / 13100) 
                             * (1 - corrections.focal_distance / 13100);
          float focal_slope = corrections.focal_slope;
          intensity += focal_slope * (abs(focal_offset - 256 * 
            (1 - tmp.uint/65535)*(1 - tmp.uint/65535)));
          intensity = (intensity < min_intensity) ? min_intensity : intensity;
          intensity = (intensity > max_intensity) ? max_intensity : intensity;
  
          if (pointInRange(distance)) {
  
            // convert polar coordinates to Euclidean XYZ
            VPoint point;
            point.ring = corrections.laser_ring;
            point.x = x_coord;
            point.y = y_coord;
            point.z = z_coord;
            point.intensity = (uint8_t) intensity;
  
            // append this point to the cloud
            pc.points.push_back(point);
            ++pc.width;
          }
        }
      }
    }
  }  

} // namespace velodyne_rawdata
