`timescale 1ns/1ps
// All five software reliability factors. Invalid evidence is never a (0,0) source.
module robust_confidence #(parameter MIN_CONFIDENCE_Q15=14746, parameter USE_SPATIAL_BACKGROUND=0)(
 input wire clk,rst_n,start,
 input wire signed [31:0] spatial_background_q23,
 input wire signed [31:0] peak_azimuth_q16,peak_elevation_q16,peak_score_q23,
 input wire [5:0] valid_channel_count,input wire [15:0] coherent_coverage_q15,
 output reg score_start,output wire score_use_sinc,score_all_groups,
 output reg signed [31:0] score_azimuth_q16,score_elevation_q16,
 input wire score_done,input wire signed [31:0] score_q23,
 output reg [15:0] phase_read_address,input wire [31:0] phase_read_value,
 output wire [5:0] quality_read_channel,input wire [15:0] quality_read_q15,
 output reg busy,done,trusted,output reg [15:0] confidence_q15,
 output reg [15:0] steered_coherence_q15,peak_contrast_q15,curvature_q15,
 output reg signed [31:0] hxx_q23,hyy_q23,hxy_q23,sidelobe_score_q23
);
 localparam IDLE=0,BEST_VECTOR_START=1,BEST_VECTOR_WAIT=2,SEED_VECTOR_START=3,SEED_VECTOR_WAIT=4,SEED_DOT=5,SIDE_REQUEST=6,SIDE_WAIT=7,SEED_NEXT=8,
  HESS_REQUEST=9,HESS_WAIT=10,HESS_CALC=11,EIGEN_START=12,EIGEN_WAIT=13,CURV_SETUP=14,CURV_START=15,CURV_WAIT=16,PSR_SETUP=17,PSR_START=18,PSR_WAIT=19,
  COH_BIN_INIT=20,COH_READ=21,COH_DELAY=22,COH_PHASE=23,COH_TRIG=24,COH_ROTATE=25,COH_ACC=26,COH_AVG_START=27,COH_AVG_WAIT=28,COH_MAG_START=29,COH_MAG_WAIT=30,COH_BIN_NEXT=31,
  COH_DIV_START=32,COH_DIV_WAIT=33,PRODUCT_INIT=34,PRODUCT_NEXT=35,ROOT_READ=36,ROOT_FINISH=37,FINISH=38;
 localparam signed [31:0] LIMIT=32'sd5898240;
 reg [5:0] state;
 reg signed [31:0] seed_az,seed_el;
 reg [3:0] hessian_index;
 reg signed [31:0] hessian_scores[0:7];
 wire score_use_sinc_internal=1'b1;assign score_use_sinc=score_use_sinc_internal;assign score_all_groups=1'b1;
 wire vector_start=(state==BEST_VECTOR_START)||(state==SEED_VECTOR_START);
 wire vector_done,vector_busy;
 wire signed [24:0] vx,vy,vz;
 wire signed [31:0] vector_az=state==BEST_VECTOR_START?peak_azimuth_q16:seed_az;
 wire signed [31:0] vector_el=state==BEST_VECTOR_START?peak_elevation_q16:seed_el;
 reg signed [24:0] best_x,best_y,best_z;
 robust_direction_vector u_vector(clk,rst_n,vector_start,vector_az,vector_el,vector_busy,vector_done,vx,vy,vz);
 wire signed [51:0] dot=vx*best_x+vy*best_y+vz*best_z;
 // cos(5 deg) * 2^46. Source excludes candidates with angular distance <5deg.
 localparam signed [51:0] COS5_Q46=52'sd70100969861163;
 function signed [31:0] clamp_angle(input signed [31:0] value);
  begin clamp_angle=value>LIMIT?LIMIT:(value< -LIMIT?-LIMIT:value);end
 endfunction
 reg signed [31:0] hess_dx,hess_dy;
 always @*begin
  hess_dx=0;hess_dy=0;
  case(hessian_index)
   0:hess_dx=-16384;1:hess_dx=16384;2:hess_dy=-16384;3:hess_dy=16384;
   4:begin hess_dx=16384;hess_dy=16384;end
   5:begin hess_dx=16384;hess_dy=-16384;end
   6:begin hess_dx=-16384;hess_dy=16384;end
   7:begin hess_dx=-16384;hess_dy=-16384;end
  endcase
 end
 wire signed [63:0] determinant=hxx_q23*hyy_q23-hxy_q23*hxy_q23;
 wire signed [32:0] diagonal_difference=$signed(hxx_q23)-$signed(hyy_q23);
 wire signed [65:0] diagonal_square=diagonal_difference*diagonal_difference;
 wire signed [63:0] cross_square=hxy_q23*hxy_q23;
 wire [63:0] eigen_radicand=diagonal_square+(cross_square<<2);
 reg signed [31:0] min_eigenvalue;
 wire signed [32:0] eigen_trace=$signed(hxx_q23)+$signed(hyy_q23);
 wire sqrt_start=(state==EIGEN_START)||(state==COH_MAG_START);
 wire sqrt_done,sqrt_busy;wire [31:0] sqrt_value;
 reg signed [15:0] average_re,average_im;
 wire signed [31:0] average_r2=average_re*average_re,average_i2=average_im*average_im;
 wire [63:0] coherence_radicand={32'd0,$unsigned(average_r2)}+{32'd0,$unsigned(average_i2)};
 robust_isqrt u_sqrt(clk,rst_n,sqrt_start,state==EIGEN_START?eigen_radicand:coherence_radicand,sqrt_busy,sqrt_done,sqrt_value);
 reg [63:0] frac_numerator,frac_denominator;
 wire frac_start=(state==CURV_START)||(state==PSR_START)||(state==COH_DIV_START);
 wire frac_busy,frac_done;wire [14:0] fraction;
 robust_fraction_divider u_fraction(clk,rst_n,frac_start,frac_numerator,frac_denominator,frac_busy,frac_done,fraction);
 function [15:0] smooth(input [14:0] x);
  reg [31:0] square;reg [16:0] slope;reg [48:0] product;
  begin square=x*x;slope=17'd98304-{x,1'b0};product=(square>>15)*slope;smooth=product>>15;end
 endfunction
 wire signed [63:0] curv_numerator=$signed(min_eigenvalue)*64'sd100000-$signed(peak_score_q23);
 wire signed [63:0] psr_numerator=($signed(peak_score_q23)-$signed(sidelobe_score_q23))*64'sd100-$signed(peak_score_q23);
 reg signed [63:0] aligned_re,aligned_im;
 reg [31:0] channel_weight_sum;
 reg [5:0] contributing_count,channel;
 reg [9:0] bin_index;
 assign quality_read_channel=channel;
 (* rom_style="block" *)reg [9:0] coordinates[0:47];
 (* rom_style="block" *)reg signed [15:0] sine[0:4095];
 (* rom_style="block" *)reg [15:0] band[0:686],coherence[0:1024],root5[0:1024];
 initial begin $readmemh("rom/estimator_channel_xy.mem",coordinates);$readmemh("rom/phase_sin_q15.mem",sine);$readmemh("rom/band_q15.mem",band);$readmemh("rom/coherence_q15.mem",coherence);$readmemh("rom/root5_q15.mem",root5);end
 reg [9:0] xy;
 wire signed [4:0] x_half=xy[9:5],y_half=xy[4:0];
 wire signed [31:0] dx=x_half*best_x,dy=y_half*best_y;
 wire signed [32:0] spatial=$signed(dx)+$signed(dy);
 reg signed [55:0] delay_product;
 wire signed [31:0] delay_q15=delay_product>>>23;
 wire signed [47:0] phase_product=delay_q15*$signed({1'b0,bin_index+10'd9});
 reg [11:0] phase_index;
 reg signed [15:0] sn,cs;
 wire signed [15:0] pr=phase_read_value[15:0],pi=phase_read_value[31:16];
 wire signed [31:0] rc=pr*cs,is=pi*sn,rs=pr*sn,ic=pi*cs;
 reg signed [16:0] rotated_re,rotated_im;
 wire signed [33:0] weighted_re=rotated_re*$signed({1'b0,quality_read_q15});
 wire signed [33:0] weighted_im=rotated_im*$signed({1'b0,quality_read_q15});
 wire channel_present=(quality_read_q15!=0)&&(phase_read_value!=0);
 wire ratio_busy,ratio_done;wire signed [15:0] normalized_re,normalized_im;
 robust_complex_ratio u_average(clk,rst_n,state==COH_AVG_START,aligned_re,aligned_im,{32'd0,channel_weight_sum}<<15,ratio_busy,ratio_done,normalized_re,normalized_im);
 reg [31:0] coherent_weight_sum;
 wire [10:0] coh_index=sqrt_value>=32768?11'd1024:((sqrt_value+16)>>5);
 wire [31:0] coh_weight=coherence[coh_index]*band[bin_index];
 reg [15:0] product_q15;
 reg [2:0] factor_index;
 reg [15:0] factor;
 always @*begin
  case(factor_index)
   0:factor=coherent_coverage_q15;
   1:factor=steered_coherence_q15;
   2:factor=peak_contrast_q15;
   3:factor=curvature_q15;
   default:factor=0;
  endcase
 end
 wire [31:0] reliability_product=product_q15*factor;
 reg [15:0] root_low,root_high;reg [4:0] root_fraction;
 wire signed [16:0] root_delta=$signed({1'b0,root_high})-$signed({1'b0,root_low});
 wire signed [22:0] root_correction=root_delta*$signed({1'b0,root_fraction});
 always @(posedge clk)begin
  if(!rst_n)begin state<=IDLE;busy<=0;done<=0;trusted<=0;confidence_q15<=0;score_start<=0;steered_coherence_q15<=0;peak_contrast_q15<=0;curvature_q15<=0;phase_read_address<=0;channel<=0;bin_index<=0;seed_az<=0;seed_el<=0;coherent_weight_sum<=0;end
  else begin
   done<=0;score_start<=0;
   case(state)
    IDLE:if(start)begin busy<=1;trusted<=0;confidence_q15<=0;sidelobe_score_q23<=32'sh80000000;state<=BEST_VECTOR_START;end
    BEST_VECTOR_START:state<=BEST_VECTOR_WAIT;
    BEST_VECTOR_WAIT:if(vector_done)begin best_x<=vx;best_y<=vy;best_z<=vz;seed_az<=-LIMIT;seed_el<=-LIMIT;
 if(USE_SPATIAL_BACKGROUND)begin sidelobe_score_q23<=spatial_background_q23;hessian_index<=0;state<=HESS_REQUEST;end
 else state<=SEED_VECTOR_START;end
    SEED_VECTOR_START:state<=SEED_VECTOR_WAIT;
    SEED_VECTOR_WAIT:if(vector_done)state<=SEED_DOT;
    SEED_DOT:if(dot<=COS5_Q46)state<=SIDE_REQUEST;else state<=SEED_NEXT;
    SIDE_REQUEST:begin score_azimuth_q16<=seed_az;score_elevation_q16<=seed_el;score_start<=1;state<=SIDE_WAIT;end
    SIDE_WAIT:if(score_done)begin if(score_q23>sidelobe_score_q23)sidelobe_score_q23<=score_q23;state<=SEED_NEXT;end
    SEED_NEXT:begin
     if(seed_az==LIMIT)begin seed_az<=-LIMIT;if(seed_el==LIMIT)begin hessian_index<=0;state<=HESS_REQUEST;end else begin seed_el<=seed_el+327680;state<=SEED_VECTOR_START;end end
     else begin seed_az<=seed_az+327680;state<=SEED_VECTOR_START;end
    end
    HESS_REQUEST:begin score_azimuth_q16<=clamp_angle(peak_azimuth_q16+hess_dx);score_elevation_q16<=clamp_angle(peak_elevation_q16+hess_dy);score_start<=1;state<=HESS_WAIT;end
    HESS_WAIT:if(score_done)begin hessian_scores[hessian_index]<=score_q23;if(hessian_index==7)state<=HESS_CALC;else begin hessian_index<=hessian_index+1;state<=HESS_REQUEST;end end
    HESS_CALC:begin
     hxx_q23<=-(hessian_scores[1]-2*peak_score_q23+hessian_scores[0])*16;
     hyy_q23<=-(hessian_scores[3]-2*peak_score_q23+hessian_scores[2])*16;
     hxy_q23<=-(hessian_scores[4]-hessian_scores[5]-hessian_scores[6]+hessian_scores[7])*4;state<=EIGEN_START;
    end
    EIGEN_START:if(hxx_q23<=0||hyy_q23<=0||determinant<=0)begin curvature_q15<=0;state<=PSR_SETUP;end else state<=EIGEN_WAIT;
    EIGEN_WAIT:if(sqrt_done)begin min_eigenvalue<=($signed(eigen_trace)-$signed({1'b0,sqrt_value}))>>>1;state<=CURV_SETUP;end
    CURV_SETUP:begin frac_numerator<=curv_numerator>0?curv_numerator:64'd0;frac_denominator<=peak_score_q23>0?$signed(peak_score_q23)*64'sd49:64'd0;state<=CURV_START;end
    CURV_START:state<=CURV_WAIT;
    CURV_WAIT:if(frac_done)begin curvature_q15<=smooth(fraction);state<=PSR_SETUP;end
    PSR_SETUP:begin frac_numerator<=psr_numerator>0?psr_numerator:64'd0;frac_denominator<=peak_score_q23>0?$signed(peak_score_q23)*64'sd14:64'd0;state<=PSR_START;end
    PSR_START:state<=PSR_WAIT;
    PSR_WAIT:if(frac_done)begin peak_contrast_q15<=smooth(fraction);bin_index<=0;coherent_weight_sum<=0;state<=COH_BIN_INIT;end
    COH_BIN_INIT:begin channel<=0;aligned_re<=0;aligned_im<=0;channel_weight_sum<=0;contributing_count<=0;state<=COH_READ;end
    COH_READ:begin phase_read_address<=channel*687+bin_index;xy<=coordinates[channel];state<=COH_DELAY;end
    COH_DELAY:begin delay_product<=spatial*24'sd114640;state<=COH_PHASE;end
    COH_PHASE:begin phase_index<=(phase_product>>>15);state<=COH_TRIG;end
    COH_TRIG:begin sn<=sine[phase_index];cs<=sine[(phase_index+12'd1024)&12'hfff];state<=COH_ROTATE;end
    COH_ROTATE:begin rotated_re<=($signed(rc)-$signed(is))>>>15;rotated_im<=($signed(rs)+$signed(ic))>>>15;state<=COH_ACC;end
    COH_ACC:begin
     if(channel_present)begin aligned_re<=aligned_re+weighted_re;aligned_im<=aligned_im+weighted_im;channel_weight_sum<=channel_weight_sum+quality_read_q15;contributing_count<=contributing_count+1;end
     if(channel==47)state<=COH_AVG_START;else begin channel<=channel+1;state<=COH_READ;end
    end
    COH_AVG_START:if(contributing_count<32||channel_weight_sum==0)state<=COH_BIN_NEXT;else state<=COH_AVG_WAIT;
    COH_AVG_WAIT:if(ratio_done)begin average_re<=normalized_re;average_im<=normalized_im;state<=COH_MAG_START;end
    COH_MAG_START:state<=COH_MAG_WAIT;
    COH_MAG_WAIT:if(sqrt_done)begin coherent_weight_sum<=coherent_weight_sum+(coh_weight>>15);state<=COH_BIN_NEXT;end
    COH_BIN_NEXT:if(bin_index==686)begin frac_numerator<=coherent_weight_sum;frac_denominator<=64'd22089349;state<=COH_DIV_START;end else begin bin_index<=bin_index+1;state<=COH_BIN_INIT;end
    COH_DIV_START:state<=COH_DIV_WAIT;
    COH_DIV_WAIT:if(frac_done)begin steered_coherence_q15<={1'b0,fraction};state<=PRODUCT_INIT;end
    PRODUCT_INIT:begin product_q15<=(valid_channel_count*32'd32768)/48;factor_index<=0;state<=PRODUCT_NEXT;end
    PRODUCT_NEXT:begin product_q15<=reliability_product>>15;if(factor_index==3)state<=ROOT_READ;else factor_index<=factor_index+1;end
    ROOT_READ:begin root_low<=root5[product_q15>>5];root_high<=root5[(product_q15>>5)>=1024?1024:(product_q15>>5)+1];root_fraction<=product_q15[4:0];state<=ROOT_FINISH;end
    ROOT_FINISH:begin confidence_q15<=root_low+(root_correction>>>5);trusted<=(root_low+(root_correction>>>5))>=MIN_CONFIDENCE_Q15;state<=FINISH;end
    FINISH:begin done<=1;busy<=0;state<=IDLE;end
    default:begin busy<=0;trusted<=0;state<=IDLE;end
   endcase
  end
 end
endmodule
