`timescale 1ns/1ps
// Literal software search order: global seed grid, then eight-neighbour sinc
// refinement, halving the step after no improvement, at most 40 iterations.
module robust_local_peak_refiner #(parameter HIGH_PRECISION=0)(
 input wire clk,rst_n,start,
 input wire signed [31:0] seed_azimuth_q16,seed_elevation_q16,
 output reg score_start,score_use_sinc,score_all_groups,
 output reg signed [31:0] score_azimuth_q16,score_elevation_q16,
 input wire score_done,
 input wire signed [31:0] score_q23,
 output reg busy,done,
 output reg signed [31:0] peak_azimuth_q16,peak_elevation_q16,peak_score_q23
);
 localparam signed [31:0] LIMIT=32'sd5898240;
 localparam signed [31:0] SEED_STEP=HIGH_PRECISION?32'sd131072:32'sd327680;
 reg [3:0] state;
 reg signed [31:0] seed_az,seed_el,best_seed_score;
 reg signed [31:0] step;
 reg signed [31:0] iteration_az,iteration_el,iteration_score;
 reg signed [2:0] az_offset,el_offset;
 reg [5:0] iteration;
 reg [1:0] small_count;
 function signed [31:0] clamp_angle(input signed [31:0] value);
  begin clamp_angle=value>LIMIT?LIMIT:(value< -LIMIT?-LIMIT:value);end
 endfunction
 wire signed [31:0] candidate_az=clamp_angle(peak_azimuth_q16+az_offset*step);
 wire signed [31:0] candidate_el=clamp_angle(peak_elevation_q16+el_offset*step);
 wire unchanged=(iteration_az==peak_azimuth_q16)&&(iteration_el==peak_elevation_q16);
 wire signed [32:0] improvement=$signed(iteration_score)-$signed(peak_score_q23);
 always @(posedge clk) begin
  if(!rst_n)begin state<=0;busy<=0;done<=0;score_start<=0;score_use_sinc<=0;score_all_groups<=0;score_azimuth_q16<=0;score_elevation_q16<=0;peak_azimuth_q16<=0;peak_elevation_q16<=0;peak_score_q23<=0;seed_az<=0;seed_el<=0;best_seed_score<=0;step<=0;iteration<=0;small_count<=0;az_offset<=0;el_offset<=0;end
  else begin
   done<=0;score_start<=0;
   case(state)
    0:if(start)begin peak_azimuth_q16<=seed_azimuth_q16;peak_elevation_q16<=seed_elevation_q16;busy<=1;state<=3;end
    1:begin score_azimuth_q16<=seed_az;score_elevation_q16<=seed_el;score_use_sinc<=0;score_all_groups<=HIGH_PRECISION!=0;score_start<=1;state<=2;end
    2:if(score_done)begin
     if(score_q23>best_seed_score)begin best_seed_score<=score_q23;peak_azimuth_q16<=seed_az;peak_elevation_q16<=seed_el;end
     if(seed_az==LIMIT)begin
      seed_az<=-LIMIT;
      if(seed_el==LIMIT)state<=3;else begin seed_el<=seed_el+SEED_STEP;state<=1;end
     end else begin seed_az<=seed_az+SEED_STEP;state<=1;end
    end
    3:begin score_azimuth_q16<=peak_azimuth_q16;score_elevation_q16<=peak_elevation_q16;score_use_sinc<=1;score_all_groups<=1;score_start<=1;state<=4;end
    4:if(score_done)begin peak_score_q23<=score_q23;step<=SEED_STEP;iteration<=0;small_count<=0;state<=5;end
    5:begin iteration_az<=peak_azimuth_q16;iteration_el<=peak_elevation_q16;iteration_score<=peak_score_q23;az_offset<=-1;el_offset<=-1;state<=6;end
    6:begin score_azimuth_q16<=candidate_az;score_elevation_q16<=candidate_el;score_start<=1;state<=7;end
    7:if(score_done)begin
     if(score_q23>iteration_score)begin iteration_score<=score_q23;iteration_az<=score_azimuth_q16;iteration_el<=score_elevation_q16;end
     if(az_offset==1)begin
      az_offset<=-1;
      if(el_offset==1)state<=8;else begin el_offset<=el_offset+1;state<=6;end
     end else begin
      if(az_offset==-1&&el_offset==0)az_offset<=1;else az_offset<=az_offset+1;
      state<=6;
     end
    end
    8:begin
     iteration<=iteration+1;
     if(unchanged)begin step<=step>>>1;small_count<=0;
      if((step>>>1)<3277||iteration==39)state<=9;else state<=5;
     end else begin
      peak_azimuth_q16<=iteration_az;peak_elevation_q16<=iteration_el;peak_score_q23<=iteration_score;
      small_count<=improvement<84?small_count+1:0;
      if((improvement<84&&small_count>=1)||iteration==39)state<=9;else state<=5;
     end
    end
    9:begin done<=1;busy<=0;state<=0;end
    default:begin state<=0;busy<=0;end
   endcase
  end
 end
endmodule
