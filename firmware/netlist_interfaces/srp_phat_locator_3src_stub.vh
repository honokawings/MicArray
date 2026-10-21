`ifndef SRP_PHAT_LOCATOR_3SRC_STUB_VH
`define SRP_PHAT_LOCATOR_3SRC_STUB_VH
(* black_box="yes" *)
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
 output wire location_valid,output wire [2:0] source_valid,
 output wire [15:0] location_x,location_y,
 output wire signed [15:0] azimuth_cdeg,elevation_cdeg,
 output wire [15:0] confidence_q15,output wire signed [31:0] location_score,
 output wire signed [15:0] source0_dir_x_q15,source0_dir_y_q15,
 output wire [15:0] source1_x,source1_y,
 output wire signed [15:0] source1_azimuth_cdeg,source1_elevation_cdeg,
 output wire [15:0] source1_confidence_q15,output wire signed [31:0] source1_score,
 output wire signed [15:0] source1_dir_x_q15,source1_dir_y_q15,
 output wire [15:0] source2_x,source2_y,
 output wire signed [15:0] source2_azimuth_cdeg,source2_elevation_cdeg,
 output wire [15:0] source2_confidence_q15,output wire signed [31:0] source2_score,
 output wire signed [15:0] source2_dir_x_q15,source2_dir_y_q15,
 output wire [31:0] frame_counter,output wire locator_busy,output wire fft_protocol_error,
 output wire heatmap_write_valid,output wire [10:0] heatmap_write_index,
 output wire [7:0] heatmap_write_value,output wire heatmap_ready,heatmap_invalidate
);
endmodule
`endif
