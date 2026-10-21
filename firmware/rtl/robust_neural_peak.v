`timescale 1ns/1ps
// Original optional bounded residual refiner. It cannot create a direction.
module robust_neural_peak #(parameter STRENGTH_Q16=19661)(
 input wire clk,rst_n,start,
 input wire signed [31:0] physical_azimuth_q16,physical_elevation_q16,physical_score_q23,
 input wire [15:0] physical_confidence_q15,steered_coherence_q15,input wire [5:0] valid_channel_count,
 input wire signed [31:0] hxx_q23,hyy_q23,hxy_q23,sidelobe_score_q23,
 output reg score_start,output wire score_use_sinc,score_all_groups,
 output reg signed [31:0] score_azimuth_q16,score_elevation_q16,
 input wire score_done,input wire signed [31:0] score_q23,
 output reg busy,done,applied,
 output reg signed [31:0] refined_azimuth_q16,refined_elevation_q16,
 output wire [31:0] neural_confidence_q16
);
 localparam IDLE=0,VAR_SETUP=1,VAR_DIV_START=2,VAR_DIV_WAIT=3,VAR_SQ_START=4,VAR_SQ_WAIT=5,RHO_SQ_START=6,RHO_SQ_WAIT=7,RHO_DIV_START=8,RHO_DIV_WAIT=9,
  PATCH_REQUEST=10,PATCH_WAIT=11,SORT=12,SORT_FINISH=13,NORM_SETUP=14,NORM_START=15,NORM_WAIT=16,DIAG_INIT=17,DIAG_WRITE=18,MODEL_START=19,MODEL_WAIT=20,
  STRENGTH=21,CORRECTION=22,SAFETY=23,CORRECTED_REQUEST=24,CORRECTED_WAIT=25,FINISH=26,
  PREP0=27,PREP1=28,PREP2=29,PREP3=30,VAR_PRODUCT=31,MAHA_MUL=32,MAHA_SUM=33,CORR_PREP=34,DIAG_DIV_START=35,DIAG_DIV_WAIT=36;
 localparam signed [31:0] LIMIT=32'sd5898240;
 reg [5:0] state;
 reg variance_axis;
 reg [63:0] division_numerator,division_denominator,variance_value;
 wire division_start=state==VAR_DIV_START||state==DIAG_DIV_START;
 wire division_busy,division_done;wire [63:0] division_value;
 robust_integer_divider u_division(clk,rst_n,division_start,division_numerator,division_denominator,division_busy,division_done,division_value);
 wire signed [31:0] hx20=hxx_q23>>>3,hy20=hyy_q23>>>3,hxy20=hxy_q23>>>3;
 reg signed [63:0] determinant20,cov_product_a,cov_product_b;
 reg signed [32:0] difference;
 reg [31:0] abs_peak,one_percent_peak,residual_floor_q23;
 reg [32:0] residual_base;
 wire [32:0] divided_residual=residual_base/33'd50;
 wire [31:0] residual20=residual_floor_q23>>3;
 reg [63:0] variance_product,rho_squared;
 wire sqrt_start=state==VAR_SQ_START||state==RHO_SQ_START;
 wire sqrt_busy,sqrt_done;wire [31:0] sqrt_value;
 robust_isqrt u_sqrt(clk,rst_n,sqrt_start,state==RHO_SQ_START?rho_squared:variance_value,sqrt_busy,sqrt_done,sqrt_value);
 reg [31:0] sigma_az_q16,sigma_el_q16,step_az_q16,step_el_q16;
 reg signed [31:0] rho_q24;
 reg [63:0] frac_numerator,frac_denominator;
 wire frac_start=state==RHO_DIV_START||state==NORM_START;
 wire frac_busy,frac_done;wire [23:0] fraction;
 robust_fraction_divider #(.F(24))u_fraction(clk,rst_n,frac_start,frac_numerator,frac_denominator,frac_busy,frac_done,fraction);
 reg [6:0] patch_index,input_index,sort_i,sort_j;
 reg signed [31:0] patch[0:80],sorted[0:80];
 reg signed [31:0] median,p95,maximum,dynamic_range;
 reg signed [31:0] center_score;
 wire [3:0] patch_row=patch_index/9,patch_column=patch_index%9;
 wire signed [5:0] column_offset=$signed({1'b0,patch_column})-6'sd4;
 wire signed [5:0] row_offset=6'sd4-$signed({1'b0,patch_row});
 function signed [31:0] clamp_angle(input signed [31:0] value);
  begin clamp_angle=value>LIMIT?LIMIT:(value< -LIMIT?-LIMIT:value);end
 endfunction
 assign score_use_sinc=1'b1;assign score_all_groups=1'b1;
 reg model_write;reg [6:0] model_address;reg signed [31:0] model_input;
 wire model_busy,model_done,model_valid;
 wire signed [31:0] cells_az,cells_el,sigma_scale_az,sigma_scale_el,correlation_residual;
 robust_neural_mlp u_model(clk,rst_n,model_write,model_address,model_input,state==MODEL_START,model_busy,model_done,model_valid,cells_az,cells_el,sigma_scale_az,sigma_scale_el,correlation_residual,neural_confidence_q16);
 (* rom_style="distributed" *)reg [12:0] log2_table[0:1023];
 initial $readmemh("rom/log2_mantissa_q12.mem",log2_table);
 function [5:0] exponent(input [63:0] value);
  integer i;begin exponent=0;for(i=0;i<64;i=i+1)if(value[i])exponent=i;end
 endfunction
 function [9:0] mantissa(input [63:0] value,input [5:0] e);
  reg [73:0] shifted;begin shifted={value,10'd0}>>e;mantissa=shifted[9:0];end
 endfunction
 reg [31:0] abs_side;
 reg [63:0] sum_for_log;
 reg [5:0] log_sum_e,log_side_e;
 reg [9:0] log_sum_index,log_side_index;
 reg [12:0] log_sum_value,log_side_value;
 reg signed [31:0] log_difference;
 reg signed [63:0] log_feature;
 reg [31:0] psr_feature_q24;
 reg [3:0] diagnostic_index;
 reg diagnostic_negative;
 wire [31:0] absolute_az=physical_azimuth_q16[31]?-$signed(physical_azimuth_q16):physical_azimuth_q16;
 wire [31:0] absolute_el=physical_elevation_q16[31]?-$signed(physical_elevation_q16):physical_elevation_q16;
 wire signed [63:0] norm_difference=input_index<=80?$signed(patch[input_index])-$signed(median):64'sd0;
 reg [31:0] strength_q16;
 wire [63:0] strength_product=neural_confidence_q16*STRENGTH_Q16;
 wire [63:0] reliability_strength=(strength_product>>16)*physical_confidence_q15;
 reg signed [31:0] correction_az_q16,correction_el_q16;
 wire signed [35:0] ca_product=$signed(cells_az[17:0])*$signed(step_az_q16[17:0]),ce_product=$signed(cells_el[17:0])*$signed(step_el_q16[17:0]);
 reg signed [17:0] cell_step_az,cell_step_el;
 wire signed [63:0] ca_strength=cell_step_az*$signed({1'b0,strength_q16}),ce_strength=cell_step_el*$signed({1'b0,strength_q16});
 wire signed [63:0] ca2=correction_az_q16*correction_az_q16,ce2=correction_el_q16*correction_el_q16,cross_c=correction_az_q16*correction_el_q16;
 wire signed [63:0] maha_x=$signed(hxx_q23)*ca2,maha_y=$signed(hyy_q23)*ce2,maha_xy=$signed(hxy_q23)*cross_c;
 reg signed [63:0] maha_x_reg,maha_y_reg,maha_xy_reg,mahalanobis_numerator;
 wire [63:0] mahalanobis_limit=({32'd0,residual_floor_q23}*25)<<30;
 wire signed [31:0] corrected_az=physical_azimuth_q16+correction_az_q16,corrected_el=physical_elevation_q16+correction_el_q16;
 wire signed [63:0] corrected_score_x100=$signed(score_q23)*64'sd100,center_score_x97=$signed(center_score)*64'sd97;
 always @(posedge clk)begin
  if(!rst_n)begin state<=IDLE;busy<=0;done<=0;applied<=0;score_start<=0;model_write<=0;refined_azimuth_q16<=0;refined_elevation_q16<=0;variance_axis<=0;patch_index<=0;input_index<=0;diagnostic_index<=0;sort_i<=0;sort_j<=0;end
  else begin done<=0;score_start<=0;model_write<=0;case(state)
   IDLE:if(start)begin busy<=1;applied<=0;refined_azimuth_q16<=physical_azimuth_q16;refined_elevation_q16<=physical_elevation_q16;variance_axis<=0;
    difference<=$signed(physical_score_q23)-$signed(sidelobe_score_q23);abs_peak<=physical_score_q23[31]?-$signed(physical_score_q23):physical_score_q23;abs_side<=sidelobe_score_q23[31]?-$signed(sidelobe_score_q23):sidelobe_score_q23;state<=PREP0;end
   PREP0:begin cov_product_a<=hx20*hy20;cov_product_b<=hxy20*hxy20;one_percent_peak<=abs_peak/32'd100;sum_for_log<={32'd0,abs_peak}+{32'd0,abs_side};state<=PREP1;end
   PREP1:begin determinant20<=cov_product_a-cov_product_b;residual_base<=difference>$signed({1'b0,one_percent_peak})?difference:{1'b0,one_percent_peak};log_sum_e<=exponent(sum_for_log);log_side_e<=exponent({32'd0,abs_side});state<=PREP2;end
   PREP2:begin residual_floor_q23<=divided_residual>0?divided_residual:32'd1;rho_squared<=$unsigned(hxx_q23)*$unsigned(hyy_q23);log_sum_index<=mantissa(sum_for_log,log_sum_e);log_side_index<=mantissa({32'd0,abs_side},log_side_e);state<=PREP3;end
   PREP3:begin log_sum_value<=log2_table[log_sum_index];log_side_value<=log2_table[log_side_index];state<=VAR_PRODUCT;end
   VAR_PRODUCT:begin variance_product<=residual20*$unsigned(variance_axis?hx20:hy20);log_difference<=($signed({1'b0,log_sum_e})-$signed({1'b0,log_side_e}))*32'sd4096+$signed({1'b0,log_sum_value})-$signed({1'b0,log_side_value});state<=VAR_SETUP;end
   VAR_SETUP:begin
    log_feature<=log_difference*64'sd11357;
    if(determinant20<=0||hx20<=0||hy20<=0||variance_product[63:40]!=0)state<=FINISH;
    else begin division_numerator<=variance_product<<24;division_denominator<=determinant20;state<=VAR_DIV_START;end
   end
   VAR_DIV_START:state<=VAR_DIV_WAIT;
   VAR_DIV_WAIT:if(division_done)begin variance_value<=division_value;state<=VAR_SQ_START;end
   VAR_SQ_START:state<=VAR_SQ_WAIT;
   VAR_SQ_WAIT:if(sqrt_done)begin
    if(sqrt_value==0)state<=FINISH;
    else if(!variance_axis)begin sigma_az_q16<=sqrt_value<<4;variance_axis<=1;state<=VAR_PRODUCT;end
    else begin sigma_el_q16<=sqrt_value<<4;state<=RHO_SQ_START;end
   end
   RHO_SQ_START:state<=RHO_SQ_WAIT;
   RHO_SQ_WAIT:if(sqrt_done)begin frac_numerator<=hxy_q23[31]?-$signed(hxy_q23):hxy_q23;frac_denominator<=sqrt_value;state<=RHO_DIV_START;end
   RHO_DIV_START:state<=RHO_DIV_WAIT;
   RHO_DIV_WAIT:if(frac_done)begin
    rho_q24<=hxy_q23[31]?(fraction>16609444?32'sd16609444:$signed({1'b0,fraction})):(fraction>16609444?-32'sd16609444:-$signed({1'b0,fraction}));
    step_az_q16<=sigma_az_q16*2/5<16384?16384:(sigma_az_q16*2/5>98304?98304:(sigma_az_q16*2+2)/5);
    step_el_q16<=sigma_el_q16*2/5<16384?16384:(sigma_el_q16*2/5>98304?98304:(sigma_el_q16*2+2)/5);
    patch_index<=0;maximum<=32'sh80000000;state<=PATCH_REQUEST;
   end
   PATCH_REQUEST:begin score_azimuth_q16<=clamp_angle(physical_azimuth_q16+column_offset*$signed(step_az_q16));score_elevation_q16<=clamp_angle(physical_elevation_q16+row_offset*$signed(step_el_q16));score_start<=1;state<=PATCH_WAIT;end
   PATCH_WAIT:if(score_done)begin
    patch[patch_index]<=score_q23;sorted[patch_index]<=score_q23;if(patch_index==40)center_score<=score_q23;if(score_q23>maximum)maximum<=score_q23;
    if(patch_index==80)begin sort_i<=0;sort_j<=0;state<=SORT;end else begin patch_index<=patch_index+1;state<=PATCH_REQUEST;end
   end
   SORT:begin
    if(sorted[sort_j]>sorted[sort_j+1])begin sorted[sort_j]<=sorted[sort_j+1];sorted[sort_j+1]<=sorted[sort_j];end
    if(sort_j==79-sort_i)begin sort_j<=0;if(sort_i==79)state<=SORT_FINISH;else sort_i<=sort_i+1;end else sort_j<=sort_j+1;
   end
   SORT_FINISH:begin median<=sorted[40];p95<=sorted[76];dynamic_range<=sorted[76]-sorted[40];input_index<=0;if(sorted[76]<=sorted[40])state<=FINISH;else state<=NORM_SETUP;end
   NORM_SETUP:begin frac_numerator<=norm_difference>0?norm_difference:64'd0;frac_denominator<=dynamic_range;state<=NORM_START;end
   NORM_START:state<=NORM_WAIT;
   NORM_WAIT:if(frac_done)begin
    model_write<=1;
    if(input_index==81)begin
     model_address<=83;model_input<=frac_numerator>=frac_denominator?32'sd16777216:$signed({1'b0,fraction});
     diagnostic_index<=3;state<=DIAG_WRITE;
    end else begin
     model_address<=input_index;model_input<=norm_difference>=dynamic_range?32'sd16777216:$signed({1'b0,fraction});
     if(input_index==80)state<=DIAG_INIT;else begin input_index<=input_index+1;state<=NORM_SETUP;end
    end
   end
   DIAG_INIT:begin
    diagnostic_index<=0;
    psr_feature_q24<=abs_side==0?32'd16777216:(log_feature>>>4)>16777216?32'd16777216:(log_feature>>>4);state<=DIAG_WRITE;
   end
   DIAG_WRITE:begin
    model_address<=81+diagnostic_index;model_write<=1;
    case(diagnostic_index)
     0:begin model_write<=0;division_numerator<={24'd0,absolute_az,8'd0};division_denominator<=90;diagnostic_negative<=physical_azimuth_q16[31];state<=DIAG_DIV_START;end
     1:begin model_write<=0;division_numerator<={24'd0,absolute_el,8'd0};division_denominator<=90;diagnostic_negative<=physical_elevation_q16[31];state<=DIAG_DIV_START;end
     2:begin model_write<=0;frac_numerator<=dynamic_range;frac_denominator<=maximum>0?maximum:-maximum;input_index<=81;state<=NORM_START;end
     3:model_input<=(step_az_q16*512+1)/3;
     4:model_input<=(step_el_q16*512+1)/3;
     5:model_input<=sigma_az_q16>=655360?32'd16777216:(sigma_az_q16*128+2)/5;
     6:model_input<=sigma_el_q16>=655360?32'd16777216:(sigma_el_q16*128+2)/5;
     7:model_input<=rho_q24;
     8:model_input<=physical_confidence_q15*512;
     9:model_input<=psr_feature_q24;
     10:model_input<=steered_coherence_q15*512;
     11:model_input<=valid_channel_count*32'd16777216/48;
    endcase
    if(diagnostic_index>2)begin if(diagnostic_index==11)state<=MODEL_START;else diagnostic_index<=diagnostic_index+1;end
   end
   DIAG_DIV_START:state<=DIAG_DIV_WAIT;
   DIAG_DIV_WAIT:if(division_done)begin model_write<=1;model_address<=81+diagnostic_index;model_input<=diagnostic_negative?-$signed(division_value[31:0]):$signed(division_value[31:0]);diagnostic_index<=diagnostic_index+1;state<=DIAG_WRITE;end
   MODEL_START:state<=MODEL_WAIT;
   MODEL_WAIT:if(model_done)begin if(!model_valid||neural_confidence_q16<36045)state<=FINISH;else state<=STRENGTH;end
   STRENGTH:begin strength_q16<=reliability_strength>>15;state<=CORR_PREP;end
   CORR_PREP:begin cell_step_az<=ca_product>>>16;cell_step_el<=ce_product>>>16;state<=CORRECTION;end
   CORRECTION:begin correction_az_q16<=ca_strength>>>16;correction_el_q16<=ce_strength>>>16;state<=MAHA_MUL;end
   MAHA_MUL:begin maha_x_reg<=maha_x;maha_y_reg<=maha_y;maha_xy_reg<=maha_xy;state<=MAHA_SUM;end
   MAHA_SUM:begin mahalanobis_numerator<=maha_x_reg+maha_y_reg+(maha_xy_reg<<1);state<=SAFETY;end
   SAFETY:begin
    if((ca2+ce2)>64'sd9663676416||mahalanobis_numerator<0||$unsigned(mahalanobis_numerator)>mahalanobis_limit||corrected_az>LIMIT||corrected_az< -LIMIT||corrected_el>LIMIT||corrected_el< -LIMIT)state<=FINISH;
    else state<=CORRECTED_REQUEST;
   end
   CORRECTED_REQUEST:begin score_azimuth_q16<=corrected_az;score_elevation_q16<=corrected_el;score_start<=1;state<=CORRECTED_WAIT;end
   CORRECTED_WAIT:if(score_done)begin if(corrected_score_x100>=center_score_x97)begin refined_azimuth_q16<=corrected_az;refined_elevation_q16<=corrected_el;applied<=1;end state<=FINISH;end
   FINISH:begin done<=1;busy<=0;state<=IDLE;end
   default:begin state<=IDLE;busy<=0;applied<=0;end
  endcase end
 end
endmodule
