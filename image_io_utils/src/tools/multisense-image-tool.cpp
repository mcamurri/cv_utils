// Tool to take a multisense stereo image (images_t)
// - republish images on seperate channels
// - use disparity image and left and create a point cloud
// - apply affordances as image mask 
//
// Features required:
// e.g. support for rgb, masking, checking if images are actually left or disparity
//
// Example usage:
// drc-multisense-image-tool -p -s -d 4

#include <stdio.h>
#include <memory>
#include <lcm/lcm.h>

#include <opencv/cv.h>
#include <opencv/highgui.h>

#include <ConciseArgs>
#include <pcl/io/pcd_io.h>
#include <pcl/point_types.h>

#include <lcmtypes/bot_core/image_t.hpp>
#include <lcmtypes/bot_core/images_t.hpp>

#include <bot_param/param_client.h>
#include <bot_frames/bot_frames.h>
#include <bot_frames_cpp/bot_frames_cpp.hpp>

#include <multisense_utils/multisense_utils.hpp> // create point clouds
#include <pronto_utils/pronto_vis.hpp> // visualize pt clds
#include <image_io_utils/image_io_utils.hpp>   // to simplify jpeg/zlib compression and decompression
#include <camera_params/camera_params.hpp>     // (Stereo) Camera Parameters

using namespace cv;
using namespace std;

class image_tool{
  public:
    image_tool(boost::shared_ptr<lcm::LCM> &lcm_, std::string camera_in_, 
               std::string camera_out_, 
               bool output_pointcloud_, bool output_images_,
               bool write_pointcloud_, bool write_images_, 
               int decimate_);
    
    ~image_tool(){}
    
  private:
    boost::shared_ptr<lcm::LCM> lcm_;
    std::string camera_in_, camera_out_;
    std::string mask_channel_;
    bool output_pointcloud_, output_images_;
    bool write_pointcloud_, write_images_;
    
    void disparityHandler(const lcm::ReceiveBuffer* rbuf, const std::string& channel, const  bot_core::images_t* msg);   
    void maskHandler(const lcm::ReceiveBuffer* rbuf, const std::string& channel, const  bot_core::image_t* msg);   
    
    int counter_;

    BotParam* botparam_;
    BotFrames* botframes_;
    bot::frames* botframes_cpp_;
    StereoParams stereo_params_;

    mutable std::vector<float> disparity_buff_;
    mutable std::vector<cv::Vec3f> points_buff_;
    mutable pcl::PCLPointCloud2 cloud_out;
    
    cv::Mat_<double> Q_;
    int decimate_;
    
    pronto_vis* pc_vis_;  
    multisense_utils* ms_utils_;     
    image_io_utils*  imgutils_;

    bot_core::image_t last_mask_;    
};    

image_tool::image_tool(boost::shared_ptr<lcm::LCM> &lcm_, std::string camera_in_,
                       std::string camera_out_, 
                       bool output_pointcloud_, bool output_images_, 
                       bool write_pointcloud_, bool write_images_, 
                       int decimate_):
      lcm_(lcm_), camera_in_(camera_in_), camera_out_(camera_out_), 
      output_pointcloud_(output_pointcloud_), output_images_(output_images_), 
      write_pointcloud_(write_pointcloud_), write_images_(write_images_), 
      decimate_(decimate_), Q_(4,4,0.0){
        
  lcm_->subscribe( camera_in_.c_str(),&image_tool::disparityHandler,this);
  mask_channel_="CAMERALEFT_MASKZIPPED";
  lcm_->subscribe( mask_channel_ ,&image_tool::maskHandler,this);

  botparam_ = bot_param_new_from_server(lcm_->getUnderlyingLCM(), 0);  
  botframes_= bot_frames_get_global(lcm_->getUnderlyingLCM(), botparam_);  
        
  stereo_params_ = StereoParams();
  stereo_params_.setParams(botparam_, "CAMERA");
  
  double baseline = fabs(stereo_params_.translation[0]);
  Q_(0,0) = Q_(1,1) = 1.0;  
  Q_(3,2) = 1.0 / baseline;
  Q_(0,3) = -stereo_params_.right.cx;
  Q_(1,3) = -stereo_params_.right.cy;
  Q_(2,3) =  stereo_params_.right.fx;
  Q_(3,3) = (stereo_params_.right.cx - stereo_params_.left.cx ) / baseline;  
  
  std::cout << Q_ << " is reprojectionMatrix\n";  
  
  imgutils_ = new image_io_utils( lcm_, stereo_params_.left.width, stereo_params_.left.height);

  pc_vis_ = new pronto_vis(lcm_->getUnderlyingLCM());
  float colors_r[] ={1.0,0.0,0.0};
  vector <float> colors_v_r;
  colors_v_r.assign(colors_r,colors_r+4*sizeof(float));
  // obj: id name type reset
  // pts: id name type reset objcoll usergb rgb
  pc_vis_->obj_cfg_list.push_back( obj_cfg(3000,"Pose",5,1) );
  pc_vis_->ptcld_cfg_list.push_back( ptcld_cfg(3001,"Pts",1,1, 3000,0,colors_v_r));
  ms_utils_ = new multisense_utils();
  ms_utils_->set_decimate( decimate_ );
  
  last_mask_.utime =0; // use this number to determine initial image
  counter_=0;  
}

void image_tool::disparityHandler(const lcm::ReceiveBuffer* rbuf, const std::string& channel, const bot_core::images_t* msg){
  int w = msg->images[0].width;
  int h = msg->images[0].height; 

  // 1. Output Images
  if (output_images_){
    // cout << (int) msg->image_types[0] << " and " << (int) msg->image_types[1] << "\n";
    lcm_->publish(camera_out_.c_str(), &msg->images[0]);
    // lcm_->publish( "SECOND_IMAGE" , &msg->images[1]); // TODO add paramater for name
  }

  // Only process the point cloud occasionally:
  counter_++;
  if (counter_ % 20 !=0){ 
    return;
  }
  cout << counter_ << " @ "<< msg->utime << " | "<< msg->images[0].width <<" x "<< msg->images[0].height <<"\n";

  //cout << msg->n_images << "\n";
  //cout << "image 0: " << msg->image_types[0] << "\n";
  //cout << "image 1: " << msg->image_types[1] << "\n";
  //cout << "image 2: " << msg->image_types[2] << "\n";

  if (!output_pointcloud_)
    return;
  
  // Extract a point cloud:
  pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud (new pcl::PointCloud<pcl::PointXYZRGB>);
  ms_utils_->unpack_multisense(msg,Q_,cloud);  
  // cout << "size ptcd: " << cloud->points.size() << " " << cloud->width << " " << cloud->height << "\n";
  
  /// 2. Colorize point cloud using the image mask
  // TODO: add proper time checks ... or change change incoming messages
  if(last_mask_.utime!=0){
    // cout <<"got mask and depth\n";
    uint8_t* mask_buf = imgutils_->unzipImage(   &last_mask_ );
    //imgutils_->sendImage(mask_buf, msg->utime, 1024, 544, 1, string("UNZIPPED")  );

    // Colorise the depth points using the mask
    int j2=0;
    for(int v=0; v<h; v=v+ decimate_) { // t2b
      for(int u=0; u<w; u=u+decimate_ ) {  //l2r
          if (mask_buf[v*w + u] > 0){ // if the mask is not black, apply it as red
            cloud->points[j2].r = mask_buf[v*w + u];
            cloud->points[j2].g = cloud->points[j2].g/4;
            cloud->points[j2].b = cloud->points[j2].b/4; // reduce other color for emphaise
          }
          j2++;
      }
    } 
      
    if (1==1){ // sanity checks
      cv::Mat mask(h, w, CV_8UC1);
      mask.data = mask_buf;
      cv::imwrite("mask_image.png", mask);

      cv::Mat img(h, w, CV_8UC1);
      std::copy(msg->images[0].data.data(), msg->images[0].data.data() + (msg->images[0].size) , img.data);
      cv::imwrite("gray_image.png", img);

      cv::Mat mask_combined;
      mask_combined = 0.3*img + 0.7*mask;
      cv::imwrite("mask_image_applied_to_gray.png", mask_combined);
      
      // cout << "size ptcd: " << cloud->points.size() << " " << cloud->width << " " << cloud->height << "\n";
    }
  }
  
  /// 3 Publish the point cloud on the camera pose:
  Eigen::Isometry3d ref_pose;
  if (1==0){ 
    // Apply in initial local-to-camera pose transform
    ref_pose.setIdentity();
    Eigen::Matrix3d m;
    m = Eigen::AngleAxisd ( 90*M_PI/180 , Eigen::Vector3d::UnitZ ()) // was 0
      * Eigen::AngleAxisd ( 180*M_PI/180 , Eigen::Vector3d::UnitY ())
      * Eigen::AngleAxisd ( 90*M_PI/180  , Eigen::Vector3d::UnitX ());
    ref_pose *= m;  
  }else{
    botframes_cpp_->get_trans_with_utime( botframes_ ,  "CAMERA_LEFT", "local", msg->utime, ref_pose);
  }
  Isometry3dTime ref_poseT = Isometry3dTime(msg->utime, ref_pose);
  pc_vis_->pose_to_lcm_from_list(3000, ref_poseT);    
  pc_vis_->ptcld_to_lcm_from_list(3001, *cloud, msg->utime, msg->utime);
  
  if (write_pointcloud_){
    pcl::PCDWriter writer;
    std::stringstream pcd_fname;
    pcd_fname << "/tmp/multisense_" << counter_ << ".pcd";
    std::cout << pcd_fname.str() << " written\n";
    writer.write (pcd_fname.str() , *cloud, false);  
    
    
    Eigen::Isometry3f ref_pose_f = ref_pose.cast<float>();
    Eigen::Quaternionf ref_pose_f_quat(ref_pose_f.rotation());
    pcl::transformPointCloud (*cloud, *cloud,
                              ref_pose_f.translation(), ref_pose_f_quat);      
    
    std::stringstream pcd_fname2;
    pcd_fname2 << "/tmp/multisense_" << counter_ << "_local.pcd";
    std::cout << pcd_fname2.str() << " written\n";
    writer.write (pcd_fname2.str() , *cloud, false);  
    
  }
}


void image_tool::maskHandler(const lcm::ReceiveBuffer* rbuf, const std::string& channel, const  bot_core::image_t* msg){
  last_mask_= *msg;  
  //cout << "got mask\n";  
}

int main(int argc, char ** argv) {
  ConciseArgs parser(argc, argv, "registeration-app");
  string camera_in="CAMERA";
  string camera_out="CAMERA_LEFT";
  bool output_pointcloud=false; // to LCM viewer
  bool output_images=false;
  bool write_pointcloud=false; // to file
  bool write_images=false;
  int decimate = 8;
  parser.add(camera_in, "i", "in", "Incoming Multisense channel");
  parser.add(camera_out, "c", "out", "Outgoing Mono Camera channel");
  parser.add(output_pointcloud, "op", "output_pointcloud", "Output PointCloud");
  parser.add(output_images, "oi", "output_images", "Output the images split");
  parser.add(write_pointcloud, "fp", "write_pointcloud", "Write PointCloud file");
  parser.add(write_images, "fi", "write_images", "Write images");
  parser.add(decimate, "d", "decimate", "Decimation of data");
  parser.parse();
  cout << camera_in << " is camera_in\n"; 
  cout << camera_out << " is camera_out\n"; 
  cout << decimate << " is decimate\n"; 
  cout << output_pointcloud << " is output_pointcloud\n"; 
  cout << output_images << " is output_images (split)\n"; 
  cout << write_pointcloud << " is write_pointcloud\n"; 
  cout << write_images << " is write_images \n"; 

  boost::shared_ptr<lcm::LCM> lcm(new lcm::LCM); 
  if(!lcm->good()){
    std::cerr <<"ERROR: lcm is not good()" <<std::endl;
  }
  
  image_tool app(lcm,camera_in, camera_out, 
                 output_pointcloud, output_images,
                 write_pointcloud, write_images, 
                 decimate);
  cout << "Ready image tool" << endl << "============================" << endl;
  while(0 == lcm->handle());
  return 0;
}
