`timescale 1ns/1ps
// Serialized reference network, signed fixed-point, original input clipping.
module robust_neural_mlp(
 input wire clk,rst_n,input wire input_write,input wire [6:0] input_address,input wire signed [31:0] input_value_q24,input wire start,
 output reg busy,done,model_output_valid,
 output reg signed [31:0] azimuth_cells_q16,elevation_cells_q16,azimuth_sigma_scale_q16,elevation_sigma_scale_q16,correlation_residual_q16,
 output reg [31:0] neural_confidence_q16
);
 localparam IDLE=0,NORM_READ=1,NORM_MULT=2,NORM_SAVE=3,TARGET_INIT=4,MAC_READ=5,MAC_ACC=6,ACT_START=7,ACT_WAIT=8,TARGET_NEXT=9,DECODE_START=10,DECODE_WAIT=11,FINISH=12;
 reg [3:0] state;reg [1:0] layer;reg [6:0] source_index;reg [5:0] target_index;reg [2:0] decode_index;
 reg signed [31:0] inputs[0:92],normalized[0:92],hidden1[0:47],hidden2[0:23],raw[0:5];
 (* rom_style="block" *)reg signed [31:0] weights[0:5759],means[0:92],inverse_scales[0:92];
 (* rom_style="distributed" *)reg signed [31:0] biases[0:77];
 initial $readmemh("rom/nn_weights_q20.mem",weights);
 initial $readmemh("rom/nn_means_q24.mem",means);
 initial $readmemh("rom/nn_inverse_scales_q12.mem",inverse_scales);
 initial $readmemh("rom/nn_biases_q20.mem",biases);
 reg signed [31:0] input_reg,mean_reg,scale_reg,source_reg,weight_reg;
 reg signed [63:0] normalization_product,accumulator;
 wire signed [32:0] centered_input=$signed(input_reg)-$signed(mean_reg);
 wire signed [63:0] normalized_value=normalization_product>>>20;
 wire [6:0] sources=layer==0?93:(layer==1?48:24);
 wire [5:0] targets=layer==0?48:(layer==1?24:6);
 wire [12:0] base=layer==0?0:(layer==1?4464:5616);
 wire [6:0] bias_base=layer==0?0:(layer==1?48:72);
 wire signed [63:0] product=source_reg*weight_reg;
 wire signed [63:0] next_sum=accumulator+product;
 reg signed [31:0] activation_value;
 wire activation_start=state==ACT_START||state==DECODE_START;
 wire signed [31:0] tanh_input=state==DECODE_START?((decode_index==2||decode_index==3||decode_index==5)?raw[decode_index]>>>1:raw[decode_index]):activation_value;
 wire tanh_busy,tanh_done;wire signed [31:0] tanh_value;
 robust_tanh u_tanh(clk,rst_n,activation_start,tanh_input,tanh_busy,tanh_done,tanh_value);
 wire signed [31:0] sigmoid=(32'sd65536+tanh_value)>>>1;
 wire signed [63:0] scale_product=sigmoid*32'sd42598;
 wire signed [63:0] correlation_product=tanh_value*32'sd22938;
 reg signed [31:0] patch_min,patch_max;
 always @(posedge clk)if(input_write&&!busy&&input_address<93)inputs[input_address]<=input_value_q24;
 always @(posedge clk)begin
  if(!rst_n)begin state<=IDLE;busy<=0;done<=0;model_output_valid<=0;source_index<=0;target_index<=0;layer<=0;decode_index<=0;azimuth_cells_q16<=0;elevation_cells_q16<=0;azimuth_sigma_scale_q16<=65536;elevation_sigma_scale_q16<=65536;correlation_residual_q16<=0;neural_confidence_q16<=0;end
  else begin done<=0;case(state)
   IDLE:if(start)begin busy<=1;model_output_valid<=0;source_index<=0;patch_min<=32'sh7fffffff;patch_max<=32'sh80000000;state<=NORM_READ;end
   NORM_READ:begin input_reg<=inputs[source_index];mean_reg<=means[source_index];scale_reg<=inverse_scales[source_index];state<=NORM_MULT;end
   NORM_MULT:begin normalization_product<=centered_input*scale_reg;state<=NORM_SAVE;end
   NORM_SAVE:begin
    normalized[source_index]<=normalized_value>524288?32'sd524288:(normalized_value< -524288?-32'sd524288:normalized_value[31:0]);
    if(source_index<81)begin if(input_reg<patch_min)patch_min<=input_reg;if(input_reg>patch_max)patch_max<=input_reg;end
    if(source_index==92)begin layer<=0;target_index<=0;state<=TARGET_INIT;end else begin source_index<=source_index+1;state<=NORM_READ;end
   end
   TARGET_INIT:begin accumulator<=$signed(biases[bias_base+target_index])*64'sd65536;source_index<=0;state<=MAC_READ;end
   MAC_READ:begin weight_reg<=weights[base+target_index*sources+source_index];source_reg<=layer==0?normalized[source_index]:(layer==1?hidden1[source_index]:hidden2[source_index]);state<=MAC_ACC;end
   MAC_ACC:begin accumulator<=next_sum;if(source_index==sources-1)begin activation_value<=next_sum>>>20;if(layer==2)begin raw[target_index]<=next_sum>>>20;state<=TARGET_NEXT;end else state<=ACT_START;end else begin source_index<=source_index+1;state<=MAC_READ;end end
   ACT_START:state<=ACT_WAIT;
   ACT_WAIT:if(tanh_done)begin if(layer==0)hidden1[target_index]<=tanh_value;else hidden2[target_index]<=tanh_value;state<=TARGET_NEXT;end
   TARGET_NEXT:begin
    if(target_index==targets-1)begin target_index<=0;if(layer==2)begin decode_index<=0;state<=DECODE_START;end else begin layer<=layer+1;state<=TARGET_INIT;end end
    else begin target_index<=target_index+1;state<=TARGET_INIT;end
   end
   DECODE_START:state<=DECODE_WAIT;
   DECODE_WAIT:if(tanh_done)begin
    case(decode_index)
     0:azimuth_cells_q16<=tanh_value;
     1:elevation_cells_q16<=tanh_value;
     2:azimuth_sigma_scale_q16<=32'sd22938+(scale_product>>>16);
     3:elevation_sigma_scale_q16<=32'sd22938+(scale_product>>>16);
     4:correlation_residual_q16<=correlation_product>>>16;
     5:neural_confidence_q16<=sigmoid;
    endcase
    if(decode_index==5)state<=FINISH;else begin decode_index<=decode_index+1;state<=DECODE_START;end
   end
   FINISH:begin model_output_valid<=patch_max-patch_min>168;done<=1;busy<=0;state<=IDLE;end
   default:begin state<=IDLE;busy<=0;model_output_valid<=0;end
  endcase end
 end
endmodule
