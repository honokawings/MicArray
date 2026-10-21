`timescale 1ns/1ps
// Raw sample IDs remain unchanged. Geometry is a separate ROM lookup.
module robust_channel_capture #(parameter N=2048)(
 input wire clk,rst_n,enable,
 input wire signed [31:0] sample_word,
 input wire sample_valid,
 input wire [5:0] sample_channel,
 input wire [31:0] read_address,
 output wire signed [23:0] read_sample,
 output reg window_done,
 output reg [31:0] completed_sample_frames,
 output reg [31:0] window_center_frame,
 output reg [31:0] rejected_groups
);
 localparam ADDRESS_BITS=$clog2(N);
 wire [23:0] bank_read[0:47];
 reg [5:0] read_channel;
 reg [5:0] expected;
 reg [$clog2(N)-1:0] frame_index;
 reg previous_enable;
 reg [31:0] window_start;
 wire valid_word=sample_valid&&sample_channel<48;
 wire accepted=enable&&rst_n&&valid_word&&(sample_channel==expected||sample_channel==0);
 wire [ADDRESS_BITS-1:0] write_index=sample_channel==expected?frame_index:{ADDRESS_BITS{1'b0}};
 genvar bank;
 generate for(bank=0;bank<48;bank=bank+1)begin:channel_bank
  (* ram_style="block" *)reg [23:0] samples[0:N-1];
  reg [23:0] read_value;
  assign bank_read[bank]=read_value;
  always @(posedge clk)begin
   read_value<=samples[read_address[ADDRESS_BITS-1:0]];
   if(accepted&&sample_channel==bank)samples[write_index]<=sample_word[23:0];
  end
 end endgenerate
 assign read_sample=read_channel<48?bank_read[read_channel]:24'd0;
 always @(posedge clk) begin
  read_channel<=read_address>>ADDRESS_BITS;
  if(!rst_n) begin expected<=0;frame_index<=0;window_done<=0;completed_sample_frames<=0;window_center_frame<=0;window_start<=0;rejected_groups<=0;previous_enable<=0;end
  else begin
   window_done<=0;previous_enable<=enable;
   if(valid_word && sample_channel==47) completed_sample_frames<=completed_sample_frames+1;
   if(!enable) begin expected<=0;frame_index<=0;end
   else if(valid_word) begin
    if(sample_channel==expected) begin
     if(expected==0 && frame_index==0) window_start<=completed_sample_frames;
     if(expected==47) begin
      expected<=0;
      if(frame_index==N-1) begin window_done<=1;window_center_frame<=window_start+N/2;frame_index<=0;end
      else frame_index<=frame_index+1;
     end else expected<=expected+1;
    end else begin
     // Discard the incomplete window. Reacquire only on a new CH0 boundary.
     if(expected!=0)rejected_groups<=rejected_groups+1;frame_index<=0;expected<=0;
     if(sample_channel==0) begin window_start<=completed_sample_frames;expected<=1;end
    end
   end
  end
 end
endmodule
