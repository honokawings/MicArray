`timescale 1ns/1ps
// Multi-source extension: shared full spatial map, local peaks, independent
// physical confidence and continuous refinement. Raw PCM IDs remain unchanged.
module srp_phat_locator_3src #(
 parameter UPDATE_FRAMES=2400,
 parameter DIRECTION_ROM="unused_legacy_direction_rom",
 parameter MIN_CONFIDENCE_Q15=14746,
 parameter MIN_SCORE=0,
 parameter NMS_RADIUS_STEPS=16,
 parameter INPUT_HANN_ENABLE=1,
 parameter NEURAL_ENABLE=0,
 parameter TEMPORAL_HISTORY=0,
 parameter HANN_COEFFICIENT_ROM="rom/hann2048.mem"
)(
 input wire clk,rst_n,input wire signed [31:0] sample_data,
 input wire sample_valid,sample_ready,input wire [5:0] sample_channel,
 output reg location_valid,output reg [2:0] source_valid,
 output reg [15:0] location_x,location_y,
 output reg signed [15:0] azimuth_cdeg,elevation_cdeg,
 output reg [15:0] confidence_q15,output reg signed [31:0] location_score,
 output reg signed [15:0] source0_dir_x_q15,source0_dir_y_q15,
 output reg [15:0] source1_x,source1_y,
 output reg signed [15:0] source1_azimuth_cdeg,source1_elevation_cdeg,
 output reg [15:0] source1_confidence_q15,output reg signed [31:0] source1_score,
 output reg signed [15:0] source1_dir_x_q15,source1_dir_y_q15,
 output reg [15:0] source2_x,source2_y,
 output reg signed [15:0] source2_azimuth_cdeg,source2_elevation_cdeg,
 output reg [15:0] source2_confidence_q15,output reg signed [31:0] source2_score,
 output reg signed [15:0] source2_dir_x_q15,source2_dir_y_q15,
 output reg [31:0] frame_counter,output wire locator_busy,output wire fft_protocol_error,
 output wire heatmap_write_valid,output wire [10:0] heatmap_write_index,
 output wire [7:0] heatmap_write_value,output reg heatmap_ready,heatmap_invalidate
);
 localparam CAPTURE=0,FRONT_START=1,FRONT_WAIT=2,QUALITIES=3,GCC_START=4,GCC_WAIT=5,PEAK_START=6,PEAK_WAIT=7,
  CONF_START=8,CONF_WAIT=9,OUT_VECTOR_START=10,OUT_VECTOR_WAIT=11,PUBLISH=12,NEURAL_START=13,NEURAL_WAIT=14,FINAL_CDEG=15,PIXEL_START=16,PIXEL_WAIT=17,LOCAL_START=18,LOCAL_WAIT=19;
 reg [4:0] state;
 assign locator_busy=state!=CAPTURE;
 wire [31:0] sample_read_address;
 wire signed [23:0] sample_read_value;
 wire capture_done;wire [31:0] sample_frames,center_frame,rejected_groups;
 reg signed [31:0] input_word;reg input_valid;reg [5:0] input_channel;
 always @(posedge clk)begin
  if(!rst_n)begin input_valid<=0;input_word<=0;input_channel<=0;end
  else begin input_valid<=sample_valid&&sample_ready;input_word<=sample_data;input_channel<=sample_channel;end
 end
 robust_channel_capture u_capture(.clk(clk),.rst_n(rst_n),.enable(state==CAPTURE),.sample_word(input_word),.sample_valid(input_valid),.sample_channel(input_channel),
  .read_address(sample_read_address),.read_sample(sample_read_value),.window_done(capture_done),.completed_sample_frames(sample_frames),.window_center_frame(center_frame),.rejected_groups(rejected_groups));
 reg history_valid,history_use;
 reg [47:0] previous_channels;
 reg [81:0] previous_groups;
 reg [31:0] previous_center,previous_rejected;
 wire [5:0] front_quality_ch;
 wire [15:0] front_quality;
 wire [47:0] front_channels;wire [5:0] front_count;
 wire phase_write;wire [15:0] phase_write_address;wire [31:0] phase_write_value;
 wire front_busy,front_done,front_valid,front_error;
 reg [5:0] quality_copy_index;
 assign front_quality_ch=quality_copy_index;
 reg [15:0] qualities[0:47];
 robust_spectral_frontend u_front(.clk(clk),.rst_n(rst_n),.start(state==FRONT_START),.sample_read_address(sample_read_address),.sample_read_value(sample_read_value),
  .quality_read_channel(front_quality_ch),.quality_read_q15(front_quality),.qualified_channels(front_channels),.qualified_count(front_count),
  .phase_write_valid(phase_write),.phase_write_address(phase_write_address),.phase_write_value(phase_write_value),.busy(front_busy),.done(front_done),.window_valid(front_valid),.fft_protocol_error(front_error));
 wire [15:0] phase_address_a,phase_address_b,smoothed_address;
 wire [31:0] phase_value_a,phase_value_b,smoothed_value;
 robust_phase_memory u_phase(.clk(clk),.rst_n(rst_n),.write_valid(phase_write),.write_address(phase_write_address),.write_value(phase_write_value),
  .history_active(history_use),.history_channels(previous_channels),.read_address_a(phase_address_a),.read_address_b(phase_address_b),.read_value_a(phase_value_a),.read_value_b(phase_value_b),
  .smoothed_read_address(smoothed_address),.smoothed_read_value(smoothed_value));
 wire [5:0] gcc_quality_a,gcc_quality_b;
 wire [14:0] gcc_read_address;wire signed [31:0] gcc_read_value;
 wire [81:0] groups;wire [6:0] group_count;
 wire group_quality_valid;wire [6:0] group_quality_index;wire [15:0] group_quality;
 wire gcc_busy,gcc_done,gcc_valid,gcc_error;
 reg [31:0] coherent_coverage_sum;
 reg [15:0] coherent_coverage;
 robust_gcc_builder u_gcc(.clk(clk),.rst_n(rst_n),.start(state==GCC_START),.history_active(history_use),.history_groups(previous_groups),
  .phase_address_a(phase_address_a),.phase_address_b(phase_address_b),.phase_value_a(phase_value_a),.phase_value_b(phase_value_b),
  .quality_channel_a(gcc_quality_a),.quality_channel_b(gcc_quality_b),.quality_a(qualities[gcc_quality_a]),.quality_b(qualities[gcc_quality_b]),
  .gcc_read_address(gcc_read_address),.gcc_read_value(gcc_read_value),.valid_groups(groups),.valid_group_count(group_count),
  .group_quality_valid(group_quality_valid),.group_quality_index(group_quality_index),.group_quality_q15(group_quality),.busy(gcc_busy),.done(gcc_done),.window_valid(gcc_valid),.fft_protocol_error(gcc_error));
 assign fft_protocol_error=front_error||gcc_error;
 wire search_score_start,search_sinc,search_all,search_busy,search_done;
 wire signed [31:0] search_az,search_el,peak_az,peak_el,peak_score;
 wire score_done;wire signed [31:0] score_value;
 reg [1:0] seed_index;
 wire [1:0] seed_count;
 wire signed [31:0] seed_az,seed_el,seed_score,background_score;
 wire spatial_score_start,spatial_sinc,spatial_all,spatial_done,spatial_busy;
 wire signed [31:0] spatial_az,spatial_el;
 robust_spatial_peaks u_spatial(
  .clk(clk),.rst_n(rst_n),.start(state==PEAK_START),
  .score_start(spatial_score_start),.score_use_sinc(spatial_sinc),.score_all_groups(spatial_all),
  .score_azimuth_q16(spatial_az),.score_elevation_q16(spatial_el),
  .score_done(score_done),.score_q23(score_value),.seed_select(seed_index),
  .seed_azimuth_q16(seed_az),.seed_elevation_q16(seed_el),.seed_score_q23(seed_score),
  .seed_count(seed_count),.background_score_q23(background_score),
  .map_write_valid(heatmap_write_valid),.map_write_index(heatmap_write_index),
  .map_write_value(heatmap_write_value),.busy(spatial_busy),.done(spatial_done));
 robust_local_peak_refiner u_search(.clk(clk),.rst_n(rst_n),.start(state==LOCAL_START),
  .seed_azimuth_q16(seed_az),.seed_elevation_q16(seed_el),
  .score_start(search_score_start),.score_use_sinc(search_sinc),.score_all_groups(search_all),
  .score_azimuth_q16(search_az),.score_elevation_q16(search_el),.score_done(score_done),.score_q23(score_value),
  .busy(search_busy),.done(search_done),.peak_azimuth_q16(peak_az),
  .peak_elevation_q16(peak_el),.peak_score_q23(peak_score));
 wire conf_score_start,conf_sinc,conf_all,conf_busy,conf_done,conf_trusted;
 wire signed [31:0] conf_az,conf_el,hxx,hyy,hxy,side_score;
 wire [15:0] conf_value,steered_coh,peak_discrimination,curvature;
 wire [5:0] conf_quality_ch;
 robust_confidence #(.MIN_CONFIDENCE_Q15(MIN_CONFIDENCE_Q15),.USE_SPATIAL_BACKGROUND(1))u_confidence(.clk(clk),.rst_n(rst_n),.start(state==CONF_START),.spatial_background_q23(background_score),
  .peak_azimuth_q16(peak_az),.peak_elevation_q16(peak_el),.peak_score_q23(peak_score),.valid_channel_count(front_count),.coherent_coverage_q15(coherent_coverage),
  .score_start(conf_score_start),.score_use_sinc(conf_sinc),.score_all_groups(conf_all),.score_azimuth_q16(conf_az),.score_elevation_q16(conf_el),.score_done(score_done),.score_q23(score_value),
  .phase_read_address(smoothed_address),.phase_read_value(smoothed_value),.quality_read_channel(conf_quality_ch),.quality_read_q15(qualities[conf_quality_ch]),
  .busy(conf_busy),.done(conf_done),.trusted(conf_trusted),.confidence_q15(conf_value),.steered_coherence_q15(steered_coh),.peak_contrast_q15(peak_discrimination),.curvature_q15(curvature),
  .hxx_q23(hxx),.hyy_q23(hyy),.hxy_q23(hxy),.sidelobe_score_q23(side_score));
 wire neural_score_start,neural_sinc,neural_all,neural_busy,neural_done,neural_applied;
 wire signed [31:0] neural_az,neural_el,refined_az,refined_el;
 wire [31:0] neural_confidence;
 generate if(NEURAL_ENABLE)begin: g_neural_enabled
 robust_neural_peak u_neural(.clk(clk),.rst_n(rst_n),.start(state==NEURAL_START),.physical_azimuth_q16(peak_az),.physical_elevation_q16(peak_el),.physical_score_q23(peak_score),
  .physical_confidence_q15(conf_value),.steered_coherence_q15(steered_coh),.valid_channel_count(front_count),.hxx_q23(hxx),.hyy_q23(hyy),.hxy_q23(hxy),.sidelobe_score_q23(side_score),
  .score_start(neural_score_start),.score_use_sinc(neural_sinc),.score_all_groups(neural_all),.score_azimuth_q16(neural_az),.score_elevation_q16(neural_el),.score_done(score_done),.score_q23(score_value),
  .busy(neural_busy),.done(neural_done),.applied(neural_applied),.refined_azimuth_q16(refined_az),.refined_elevation_q16(refined_el),.neural_confidence_q16(neural_confidence));
 end else begin: g_neural_disabled
  assign neural_score_start=1'b0;assign neural_sinc=1'b0;assign neural_all=1'b0;
  assign neural_az=0;assign neural_el=0;assign neural_busy=1'b0;
  assign neural_done=1'b0;assign neural_applied=1'b0;
  assign refined_az=peak_az;assign refined_el=peak_el;assign neural_confidence=0;
 end endgenerate
 reg signed [31:0] final_az_q16,final_el_q16;
 wire select_conf=state==CONF_START||state==CONF_WAIT;
 wire select_neural=state==NEURAL_START||state==NEURAL_WAIT;
 wire select_spatial=state==PEAK_START||state==PEAK_WAIT;
 wire score_start=select_neural?neural_score_start:(select_conf?conf_score_start:(select_spatial?spatial_score_start:search_score_start));
 wire [31:0] score_az=select_neural?neural_az:(select_conf?conf_az:(select_spatial?spatial_az:search_az)),score_el=select_neural?neural_el:(select_conf?conf_el:(select_spatial?spatial_el:search_el));
 wire score_sinc=select_neural?neural_sinc:(select_conf?conf_sinc:(select_spatial?spatial_sinc:search_sinc)),score_all=select_neural?neural_all:(select_conf?conf_all:(select_spatial?spatial_all:search_all));
 wire scorer_busy;wire signed [24:0] score_x,score_y,score_z;
 robust_srp_score u_score(.clk(clk),.rst_n(rst_n),.start(score_start),.use_sinc(score_sinc),.use_all_groups(score_all),.azimuth_q16(score_az),.elevation_q16(score_el),.valid_groups(groups),
  .gcc_read_address(gcc_read_address),.gcc_read_value(gcc_read_value),.busy(scorer_busy),.done(score_done),.score_q23(score_value),.direction_x_q23(score_x),.direction_y_q23(score_y),.direction_z_q23(score_z));
 wire output_vector_done,output_vector_busy;wire signed [24:0] output_x,output_y,output_z;
 robust_direction_vector u_output_vector(clk,rst_n,state==OUT_VECTOR_START,final_az_q16,final_el_q16,output_vector_busy,output_vector_done,output_x,output_y,output_z);
 reg publish_trusted;
 reg [2:0] working_valid;
 reg [15:0] working_x[0:2],working_y[0:2],working_confidence[0:2];
 reg signed [15:0] working_az[0:2],working_el[0:2],working_dx[0:2],working_dy[0:2];
 reg signed [31:0] working_score[0:2];
 reg signed [24:0] working_vx[0:2],working_vy[0:2],working_vz[0:2];
 wire signed [51:0] accepted_dot0=output_x*working_vx[0]+output_y*working_vy[0]+output_z*working_vz[0];
 wire signed [51:0] accepted_dot1=output_x*working_vx[1]+output_y*working_vy[1]+output_z*working_vz[1];
 wire duplicate_direction=(working_valid[0]&&accepted_dot0>52'sd68831018284033)||
                          (working_valid[1]&&accepted_dot1>52'sd68831018284033);
 integer slot;

 function signed [15:0] centidegrees(input signed [31:0] angle);
  reg [23:0] absolute;reg [31:0] c;begin absolute=angle<0?-$signed(angle):angle;c=(absolute*32'd100+32'd32768)>>16;centidegrees=angle<0?-$signed(c):c;end
 endfunction
 reg signed [15:0] final_azimuth,final_elevation;
 wire [15:0] absolute_azimuth=final_azimuth[15]?-$signed(final_azimuth):final_azimuth,absolute_elevation=final_elevation[15]?-$signed(final_elevation):final_elevation;
 wire [19:0] x_numerator=absolute_azimuth*20'd16,y_numerator=absolute_elevation*20'd12;
 wire x_busy,x_done,y_busy,y_done;wire [19:0] pixel_x_quotient,pixel_y_quotient;
 robust_integer_divider #(.W(20))u_pixel_x(clk,rst_n,state==PIXEL_START,x_numerator,20'd100,x_busy,x_done,pixel_x_quotient);
 robust_integer_divider #(.W(20))u_pixel_y(clk,rst_n,state==PIXEL_START,y_numerator,20'd100,y_busy,y_done,pixel_y_quotient);
 wire signed [31:0] diagnostic_x=32'sd960+(final_azimuth[15]?-$signed({1'b0,pixel_x_quotient}):$signed({1'b0,pixel_x_quotient}));
 wire signed [31:0] diagnostic_y=32'sd540-(final_elevation[15]?-$signed({1'b0,pixel_y_quotient}):$signed({1'b0,pixel_y_quotient}));
 always @(posedge clk)begin
  if(!rst_n)begin
   state<=CAPTURE;location_valid<=0;source_valid<=0;location_x<=960;location_y<=540;azimuth_cdeg<=0;elevation_cdeg<=0;confidence_q15<=0;location_score<=0;source0_dir_x_q15<=0;source0_dir_y_q15<=0;frame_counter<=0;
   source1_x<=0;source1_y<=0;source1_azimuth_cdeg<=0;source1_elevation_cdeg<=0;source1_confidence_q15<=0;source1_score<=0;source1_dir_x_q15<=0;source1_dir_y_q15<=0;
   source2_x<=0;source2_y<=0;source2_azimuth_cdeg<=0;source2_elevation_cdeg<=0;source2_confidence_q15<=0;source2_score<=0;source2_dir_x_q15<=0;source2_dir_y_q15<=0;
   seed_index<=0;working_valid<=0;heatmap_ready<=0;heatmap_invalidate<=0;
   for(slot=0;slot<3;slot=slot+1)begin working_x[slot]<=0;working_y[slot]<=0;working_confidence[slot]<=0;working_az[slot]<=0;working_el[slot]<=0;working_dx[slot]<=0;working_dy[slot]<=0;working_score[slot]<=0;working_vx[slot]<=0;working_vy[slot]<=0;working_vz[slot]<=0;end
   history_valid<=0;history_use<=0;previous_channels<=0;previous_groups<=0;previous_center<=0;previous_rejected<=0;quality_copy_index<=0;coherent_coverage_sum<=0;coherent_coverage<=0;publish_trusted<=0;final_az_q16<=0;final_el_q16<=0;final_azimuth<=0;final_elevation<=0;
  end else begin
   location_valid<=0;heatmap_ready<=0;heatmap_invalidate<=0;
   if(group_quality_valid)coherent_coverage_sum<=coherent_coverage_sum+group_quality;
   case(state)
    CAPTURE:if(capture_done)begin working_valid<=0;seed_index<=0;history_use<=TEMPORAL_HISTORY&&history_valid&&(center_frame-previous_center<=48000)&&(rejected_groups==previous_rejected);publish_trusted<=0;state<=FRONT_START;end
    FRONT_START:state<=FRONT_WAIT;
    FRONT_WAIT:if(front_done)begin
     if(!front_valid)begin history_valid<=0;previous_channels<=0;previous_groups<=0;publish_trusted<=0;state<=PUBLISH;end
     else begin quality_copy_index<=0;state<=QUALITIES;end
    end
    QUALITIES:begin qualities[quality_copy_index]<=front_quality;if(quality_copy_index==47)state<=GCC_START;else quality_copy_index<=quality_copy_index+1;end
    GCC_START:begin coherent_coverage_sum<=0;state<=GCC_WAIT;end
    GCC_WAIT:if(gcc_done)begin
     coherent_coverage<=coherent_coverage_sum[21:0]/22'd82;
     if(!gcc_valid)begin history_valid<=0;previous_channels<=0;previous_groups<=0;publish_trusted<=0;state<=PUBLISH;end
     else begin history_valid<=1;previous_center<=center_frame;previous_rejected<=rejected_groups;state<=PEAK_START;end
    end
    PEAK_START:begin heatmap_invalidate<=1;state<=PEAK_WAIT;end
    PEAK_WAIT:if(spatial_done)state<=seed_count==0?PUBLISH:LOCAL_START;
    LOCAL_START:state<=LOCAL_WAIT;
    LOCAL_WAIT:if(search_done)state<=CONF_START;
    CONF_START:state<=CONF_WAIT;
    CONF_WAIT:if(conf_done)begin publish_trusted<=conf_trusted;previous_channels<=front_channels;previous_groups<=groups;final_az_q16<=peak_az;final_el_q16<=peak_el;state<=(NEURAL_ENABLE&&conf_trusted)?NEURAL_START:OUT_VECTOR_START;end
    NEURAL_START:state<=NEURAL_WAIT;
    NEURAL_WAIT:if(neural_done)begin final_az_q16<=refined_az;final_el_q16<=refined_el;state<=OUT_VECTOR_START;end
    OUT_VECTOR_START:state<=OUT_VECTOR_WAIT;
    OUT_VECTOR_WAIT:if(output_vector_done)begin if(duplicate_direction)publish_trusted<=0;state<=FINAL_CDEG;end
    FINAL_CDEG:begin final_azimuth<=centidegrees(final_az_q16);final_elevation<=centidegrees(final_el_q16);state<=PIXEL_START;end
    PIXEL_START:state<=PIXEL_WAIT;
    PIXEL_WAIT:if(x_done&&y_done)begin
     if(publish_trusted)begin
      working_valid[seed_index]<=1;
      working_az[seed_index]<=final_azimuth;working_el[seed_index]<=final_elevation;
      working_confidence[seed_index]<=conf_value;working_score[seed_index]<=peak_score;
      working_dx[seed_index]<=output_x>>>8;working_dy[seed_index]<=output_y>>>8;
      working_vx[seed_index]<=output_x;working_vy[seed_index]<=output_y;working_vz[seed_index]<=output_z;
      working_x[seed_index]<=diagnostic_x<0?0:(diagnostic_x>1919?1919:diagnostic_x[15:0]);
      working_y[seed_index]<=diagnostic_y<0?0:(diagnostic_y>1079?1079:diagnostic_y[15:0]);
     end
     if(seed_index+1>=seed_count)state<=PUBLISH;
     else begin seed_index<=seed_index+1;state<=LOCAL_START;end
    end
    PUBLISH:begin
     frame_counter<=frame_counter+1;location_valid<=1;source_valid<=working_valid;heatmap_ready<=1;
     confidence_q15<=working_valid[0]?working_confidence[0]:0;location_score<=working_valid[0]?working_score[0]:0;
     location_x<=working_x[0];location_y<=working_y[0];azimuth_cdeg<=working_az[0];elevation_cdeg<=working_el[0];
     source0_dir_x_q15<=working_dx[0];source0_dir_y_q15<=working_dy[0];
     source1_confidence_q15<=working_valid[1]?working_confidence[1]:0;source1_score<=working_valid[1]?working_score[1]:0;
     source1_x<=working_x[1];source1_y<=working_y[1];source1_azimuth_cdeg<=working_az[1];source1_elevation_cdeg<=working_el[1];
     source1_dir_x_q15<=working_dx[1];source1_dir_y_q15<=working_dy[1];
     source2_confidence_q15<=working_valid[2]?working_confidence[2]:0;source2_score<=working_valid[2]?working_score[2]:0;
     source2_x<=working_x[2];source2_y<=working_y[2];source2_azimuth_cdeg<=working_az[2];source2_elevation_cdeg<=working_el[2];
     source2_dir_x_q15<=working_dx[2];source2_dir_y_q15<=working_dy[2];
     state<=CAPTURE;
    end
    default:begin state<=CAPTURE;history_valid<=0;publish_trusted<=0;source_valid<=0;end
   endcase
  end
 end
endmodule
