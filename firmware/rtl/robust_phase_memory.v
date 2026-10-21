`timescale 1ns/1ps
module robust_phase_memory(
 input wire clk,rst_n,
 input wire write_valid,input wire [15:0] write_address,input wire [31:0] write_value,
 input wire history_active,input wire [47:0] history_channels,
 input wire [15:0] read_address_a,read_address_b,
 output wire [31:0] read_value_a,read_value_b,
 input wire [15:0] smoothed_read_address,output wire [31:0] smoothed_read_value
);
 reg pending=0,use_previous;
 reg [15:0] pending_address;
 reg [31:0] pending_value;
 reg pending2=0,pending3=0,use_previous2,use_previous3;
 reg [15:0] address2,address3;
 reg [31:0] raw2,raw3;
 reg signed [33:0] previous_product_re,previous_product_im,current_product_re,current_product_im;
 reg signed [34:0] mixed_re,mixed_im;
 wire [15:0] smooth_address=write_valid?write_address:smoothed_read_address;
 wire signed [15:0] previous_re=smoothed_read_value[15:0],previous_im=smoothed_read_value[31:16];
 wire signed [15:0] new_re=pending_value[15:0],new_im=pending_value[31:16];
 wire signed [15:0] smooth_re=mixed_re>>>15,smooth_im=mixed_im>>>15;
 robust_sdp_ram u_current_a(clk,write_valid&&rst_n,write_address,write_value,read_address_a,read_value_a);
 robust_sdp_ram u_current_b(clk,write_valid&&rst_n,write_address,write_value,read_address_b,read_value_b);
 robust_sdp_ram u_smoothed(clk,pending3&&rst_n,address3,use_previous3?{smooth_im,smooth_re}:raw3,smooth_address,smoothed_read_value);
 always @(posedge clk)begin
  if(!rst_n)begin pending<=0;pending2<=0;pending3<=0;end
  else begin
  pending<=write_valid;pending2<=pending;pending3<=pending2;
  if(write_valid)begin
   pending_address<=write_address;pending_value<=write_value;
   use_previous<=history_active&&history_channels[write_address/687];
  end
  if(pending)begin
   previous_product_re<=previous_re*18'sd21299;previous_product_im<=previous_im*18'sd21299;
   current_product_re<=new_re*18'sd11469;current_product_im<=new_im*18'sd11469;
   address2<=pending_address;raw2<=pending_value;use_previous2<=use_previous;
  end
  if(pending2)begin
   mixed_re<=$signed(previous_product_re)+$signed(current_product_re);mixed_im<=$signed(previous_product_im)+$signed(current_product_im);
   address3<=address2;raw3<=raw2;use_previous3<=use_previous2;
  end
  end
 end
endmodule
