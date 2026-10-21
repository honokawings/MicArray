// Copyright 1986-2020 Xilinx, Inc. All Rights Reserved.
// --------------------------------------------------------------------------------
// Tool Version: Vivado v.2020.2 (win64) Build 3064766 Wed Nov 18 09:12:45 MST 2020
// Date        : Tue Oct 20 23:09:21 2026
// Host        : Maple running 64-bit major release  (build 9200)
// Command     : write_verilog -force -mode synth_stub
//               E:/workspace/micarray_denoise_night_20261020/integration_dcp/agc_stub.v
// Design      : denoise_agc64
// Purpose     : Stub declaration of top-level module interface
// Device      : xc7z035ffg676-2
// --------------------------------------------------------------------------------

// This empty module with port declaration file causes synthesis tools to infer a black box for IP.
// The synthesis directives are for Synopsys Synplify support to prevent IO buffer insertion.
// Please paste the declaration into a Verilog source file or add the file as an additional source.
module denoise_agc64(ap_clk, ap_rst, ap_start, ap_done, ap_idle, 
  ap_ready, reset_state, sample_valid, sample, attack, release_r, target_amplitude, minimum_gain, 
  maximum_gain, limiter_amplitude, result, result_ap_vld)
/* synthesis syn_black_box black_box_pad_pin="ap_clk,ap_rst,ap_start,ap_done,ap_idle,ap_ready,reset_state,sample_valid,sample[31:0],attack[63:0],release_r[63:0],target_amplitude[63:0],minimum_gain[63:0],maximum_gain[63:0],limiter_amplitude[63:0],result[31:0],result_ap_vld" */;
  input ap_clk;
  input ap_rst;
  input ap_start;
  output ap_done;
  output ap_idle;
  output ap_ready;
  input reset_state;
  input sample_valid;
  input [31:0]sample;
  input [63:0]attack;
  input [63:0]release_r;
  input [63:0]target_amplitude;
  input [63:0]minimum_gain;
  input [63:0]maximum_gain;
  input [63:0]limiter_amplitude;
  output [31:0]result;
  output result_ap_vld;
endmodule
