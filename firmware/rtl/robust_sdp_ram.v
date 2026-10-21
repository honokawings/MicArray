`timescale 1ns/1ps
// Isolate the synchronous read register from arithmetic DSP absorption.
(* keep_hierarchy="yes" *)
module robust_sdp_ram #(parameter WIDTH=32,DEPTH=32976,ADDRESS_BITS=$clog2(DEPTH))(
 input wire clk,write_enable,input wire [ADDRESS_BITS-1:0] write_address,input wire [WIDTH-1:0] write_data,
 input wire [ADDRESS_BITS-1:0] read_address,output reg [WIDTH-1:0] read_data
);
 (* ram_style="block" *)reg [WIDTH-1:0] memory[0:DEPTH-1];
 always @(posedge clk)begin
  if(write_enable)memory[write_address]<=write_data;
  read_data<=memory[read_address];
 end
endmodule
