`timescale 1ns/1ps
// Fixed original demonstration profile only. Input/result buffers use BRAM;
// the output FIFO uses distributed memory and keeps the original large depth.
// 1024-sample blocking never silently drops/flushes the incomplete final block.
// BP runs in parallel with Log/AGC; two input banks keep the Log input immutable.
module denoise_post_profile_v3 #(
 parameter FIFO_BITS=11,
 parameter MAX_BLOCK_CYCLES=2133333
)(
 input wire clk,rst_n,
 input wire in_valid,output wire in_ready,
 input wire [31:0] in_sample,in_sample_index,
 output wire out_valid,input wire out_ready,
 output wire [31:0] out_sample,out_sample_index,
 output wire ready,output wire fault,
 output reg [31:0] fault_bits,output reg [31:0] log_numeric_fault,
 output reg [31:0] blocks_completed,max_block_cycles
);
 localparam FIFO_DEPTH=1<<FIFO_BITS;
 (* ram_style="block" *) reg [31:0] input_banks[0:2047];
 (* ram_style="block" *) reg [31:0] log_result[0:1023];
 (* ram_style="distributed" *) reg [31:0] output_fifo[0:FIFO_DEPTH-1];
 reg [1:0] bank_ready;
 reg fill_bank,log_bank,next_log_bank;
 reg [9:0] fill_offset;
 reg [31:0] expected_input,published_index;
 reg [FIFO_BITS-1:0] fifo_write,fifo_read;
 reg [FIFO_BITS:0] fifo_count;
 wire fifo_pop=out_valid&&out_ready;
 assign out_valid=(fifo_count!=0)&&ready&&!fault;
 assign out_sample=output_fifo[fifo_read];
 assign out_sample_index=published_index;
 assign fault=|fault_bits;

 reg bp_start,bp_reset,bp_initialized,bp_seen;
 reg [2:0] bp_state;
 reg [31:0] bp_sample;
 wire bp_done,bp_idle,bp_ap_ready,bp_result_valid;
 wire [31:0] bp_result;
 assign in_ready=ready&&!fault&&bp_state==2&&!bank_ready[fill_bank];
 wire accept_input=in_valid&&in_ready;
 wire bp_write=bp_state==3&&bp_result_valid&&!bp_seen;

 reg log_start,log_reset,log_initialized;
 wire log_done,log_idle,log_ap_ready;
 wire [9:0] log_input_address,log_output_address;
 wire log_input_ce,log_output_ce,log_output_we;
 reg [31:0] log_input_q;
 wire [31:0] log_output_d,log_numeric;
 wire log_numeric_valid;
 always @(posedge clk)begin
  if(log_input_ce)
   log_input_q<=input_banks[{log_bank,log_input_address}];
  if(log_output_ce&&log_output_we)log_result[log_output_address]<=log_output_d;
 end

 reg agc_start,agc_reset,agc_initialized,agc_seen;
 reg [31:0] agc_sample;
 wire agc_done,agc_idle,agc_ap_ready,agc_result_valid;
 wire [31:0] agc_result;
 reg [9:0] agc_index;
 reg [2:0] post_state;
 reg [31:0] block_cycles;
 localparam INIT_START=0,INIT_WAIT=1,WAIT_BANK=2,WAIT_LOG=3,START_AGC=4,WAIT_AGC=5;
 assign ready=bp_initialized&&log_initialized&&agc_initialized;
 wire fifo_push=post_state==WAIT_AGC&&agc_result_valid&&!agc_seen;
 wire release_bank=post_state==WAIT_LOG&&log_done;
 denoise_bandpass64 u_bandpass(
  .ap_clk(clk),.ap_rst(!rst_n),.ap_start(bp_start),.ap_done(bp_done),.ap_idle(bp_idle),.ap_ready(bp_ap_ready),
  .reset_state(bp_reset),.sample_valid(1'b1),.sample(bp_sample),
  .hp_b0(64'h3fefb4864e1ee92c),
  .hp_b1(64'hbfffb4864e1ee92c),
  .hp_b2(64'h3fefb4864e1ee92c),
  .hp_a1(64'hbfffb42d4b82cfd3),
  .hp_a2(64'h3fef69bea176050c),
  .lp_b0(64'h3fc3d8b64657cae8),
  .lp_b1(64'h3fd3d8b64657cae8),
  .lp_b2(64'h3fc3d8b64657cae8),
  .lp_a1(64'hbfe3d8b64657caea),
  .lp_a2(64'h3fcec5b232be5748),
  .result(bp_result),.result_ap_vld(bp_result_valid));
 logmmse_hop u_logmmse(
  .ap_clk(clk),.ap_rst(!rst_n),.ap_start(log_start),.ap_done(log_done),.ap_idle(log_idle),.ap_ready(log_ap_ready),
  .reset_state(log_reset),
  .samples_address0(log_input_address),.samples_ce0(log_input_ce),.samples_q0(log_input_q),
  .result_address0(log_output_address),.result_ce0(log_output_ce),.result_we0(log_output_we),.result_d0(log_output_d),
  .alpha(64'h3fef5c28f5c28f5c),
  .beta(64'h3fee666666666666),
  .min_gain(64'h3fb999999999999a),
  .numeric_fault(log_numeric),.numeric_fault_ap_vld(log_numeric_valid));
 denoise_agc64 u_agc(
  .ap_clk(clk),.ap_rst(!rst_n),.ap_start(agc_start),.ap_done(agc_done),.ap_idle(agc_idle),.ap_ready(agc_ap_ready),
  .reset_state(agc_reset),.sample_valid(1'b1),.sample(agc_sample),
  .attack(64'h3feff7789aa2fcb2),
  .release_r(64'h3fefff6e5e97a010),
  .target_amplitude(64'h3fb0270ac3f8a9f9),
  .minimum_gain(64'h3fb999999999999b),
  .maximum_gain(64'h4023ffffffffffff),
  .limiter_amplitude(64'h3fec8520affa0a4b),
  .result(agc_result),.result_ap_vld(agc_result_valid));

 always @(posedge clk)begin
  if(!rst_n)begin
   bp_start<=0;bp_reset<=1;bp_initialized<=0;bp_seen<=0;bp_state<=0;bp_sample<=0;
   fill_bank<=0;fill_offset<=0;expected_input<=0;bank_ready<=0;
   log_start<=0;log_reset<=1;log_initialized<=0;log_bank<=0;next_log_bank<=0;
   agc_start<=0;agc_reset<=1;agc_initialized<=0;agc_seen<=0;agc_sample<=0;agc_index<=0;
   post_state<=INIT_START;block_cycles<=0;blocks_completed<=0;max_block_cycles<=0;
   fifo_write<=0;fifo_read<=0;fifo_count<=0;published_index<=0;
   fault_bits<=0;log_numeric_fault<=0;
  end else begin
   bp_start<=0;log_start<=0;agc_start<=0;
   if(log_numeric_valid&&log_numeric!=0)begin
    log_numeric_fault<=log_numeric_fault|log_numeric;fault_bits[4]<=1;
   end
   if(release_bank)bank_ready[log_bank]<=0;
   if(bp_write)begin
    input_banks[{fill_bank,fill_offset}]<=bp_result;
    bp_seen<=1;
    if(fill_offset==1023)begin
     bank_ready[fill_bank]<=1;fill_bank<=!fill_bank;fill_offset<=0;
    end else fill_offset<=fill_offset+1'b1;
   end
   case(bp_state)
    0:begin bp_reset<=1;bp_start<=1;bp_state<=1;end
    1:if(bp_done)begin bp_initialized<=1;bp_reset<=0;bp_state<=2;end
    2:if(accept_input)begin
     if(in_sample_index!=expected_input)fault_bits[0]<=1;
     expected_input<=expected_input+1'b1;bp_sample<=in_sample;bp_seen<=0;bp_start<=1;bp_state<=3;
    end
    3:if(bp_done)begin
     if(!bp_seen&&!bp_result_valid)fault_bits[2]<=1;
     bp_state<=2;
    end
    default:begin bp_state<=0;fault_bits[2]<=1;end
   endcase

   if(fifo_push)begin
    if(fifo_count<FIFO_DEPTH||fifo_pop)begin
     output_fifo[fifo_write]<=agc_result;fifo_write<=fifo_write+1'b1;
    end else fault_bits[1]<=1;
    agc_seen<=1;
   end
   if(fifo_pop)begin fifo_read<=fifo_read+1'b1;published_index<=published_index+1'b1;end
   case({fifo_push,fifo_pop})
    2'b10:if(fifo_count<FIFO_DEPTH)fifo_count<=fifo_count+1'b1;
    2'b01:fifo_count<=fifo_count-1'b1;
    default:begin end
   endcase
   if(post_state==WAIT_LOG||post_state==START_AGC||post_state==WAIT_AGC)begin
    block_cycles<=block_cycles+1'b1;
    if(block_cycles>=MAX_BLOCK_CYCLES)fault_bits[5]<=1;
   end
   case(post_state)
    INIT_START:begin
     log_start<=1;log_reset<=1;agc_start<=1;agc_reset<=1;post_state<=INIT_WAIT;
    end
    INIT_WAIT:begin
     if(log_done)begin log_initialized<=1;log_reset<=0;end
     if(agc_done)begin agc_initialized<=1;agc_reset<=0;end
     if((log_initialized||log_done)&&(agc_initialized||agc_done))post_state<=WAIT_BANK;
    end
    WAIT_BANK:if(ready&&!fault&&bank_ready[next_log_bank])begin
     log_bank<=next_log_bank;next_log_bank<=!next_log_bank;
     log_start<=1;block_cycles<=0;post_state<=WAIT_LOG;
    end
    WAIT_LOG:if(log_done)begin agc_index<=0;post_state<=START_AGC;end
    START_AGC:if(!fault&&fifo_count<FIFO_DEPTH-1)begin
     agc_sample<=log_result[agc_index];agc_seen<=0;agc_start<=1;post_state<=WAIT_AGC;
    end
    WAIT_AGC:if(agc_done)begin
     if(!agc_seen&&!agc_result_valid)fault_bits[3]<=1;
     if(agc_index==1023)begin
      blocks_completed<=blocks_completed+1'b1;
      if(block_cycles+1>max_block_cycles)max_block_cycles<=block_cycles+1'b1;
      post_state<=WAIT_BANK;
     end else begin agc_index<=agc_index+1'b1;post_state<=START_AGC;end
    end
    default:begin post_state<=INIT_START;fault_bits[5]<=1;end
   endcase
  end
 end
endmodule
