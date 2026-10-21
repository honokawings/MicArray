// Copyright 1986-2020 Xilinx, Inc. All Rights Reserved.
// --------------------------------------------------------------------------------
// Tool Version: Vivado v.2020.2 (win64) Build 3064766 Wed Nov 18 09:12:45 MST 2020
// Date        : Tue Oct 20 23:22:52 2026
// Host        : Maple running 64-bit major release  (build 9200)
// Command     : write_verilog -force -mode synth_stub
//               E:/workspace/micarray_denoise_night_20261020/integration_dcp/R5_1p/log_stub.v
// Design      : logmmse_hop
// Purpose     : Stub declaration of top-level module interface
// Device      : xc7z035ffg676-2
// --------------------------------------------------------------------------------

// This empty module with port declaration file causes synthesis tools to infer a black box for IP.
// The synthesis directives are for Synopsys Synplify support to prevent IO buffer insertion.
// Please paste the declaration into a Verilog source file or add the file as an additional source.
module logmmse_hop(ap_clk, ap_rst, ap_start, ap_done, ap_idle, 
  ap_ready, reset_state, samples_address0, samples_ce0, samples_q0, result_address0, 
  result_ce0, result_we0, result_d0, alpha, beta, min_gain, numeric_fault, numeric_fault_ap_vld)
/* synthesis syn_black_box black_box_pad_pin="ap_clk,ap_rst,ap_start,ap_done,ap_idle,ap_ready,reset_state,samples_address0[9:0],samples_ce0,samples_q0[31:0],result_address0[9:0],result_ce0,result_we0,result_d0[31:0],alpha[63:0],beta[63:0],min_gain[63:0],numeric_fault[31:0],numeric_fault_ap_vld" */;
  input ap_clk;
  input ap_rst;
  input ap_start;
  output ap_done;
  output ap_idle;
  output ap_ready;
  input reset_state;
  output [9:0]samples_address0;
  output samples_ce0;
  input [31:0]samples_q0;
  output [9:0]result_address0;
  output result_ce0;
  output result_we0;
  output [31:0]result_d0;
  input [63:0]alpha;
  input [63:0]beta;
  input [63:0]min_gain;
  output [31:0]numeric_fault;
  output numeric_fault_ap_vld;
endmodule
