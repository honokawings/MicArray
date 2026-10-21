`timescale 1ns/1ps
// Same all-baseline objective as RobustSrpPhatScorer. GCC is Q8.23.
// Coarse search uses Catmull-Rom; continuous refinement uses normalized sinc8.
module robust_srp_score(
 input wire clk,rst_n,start,use_sinc,use_all_groups,
 input wire signed [31:0] azimuth_q16,elevation_q16,
 input wire [81:0] valid_groups,
 output wire [14:0] gcc_read_address,
 input wire signed [31:0] gcc_read_value,
 output reg busy,done,
 output reg signed [31:0] score_q23,
 output wire signed [24:0] direction_x_q23,direction_y_q23,direction_z_q23
);
 (* rom_style="distributed" *) reg [27:0] group_table[0:81];
 (* rom_style="block" *) reg signed [20:0] sinc_table[0:8199],cubic_table[0:4099];
 (* rom_style="distributed" *) reg [24:0] inverse_count[0:82];
 initial begin
  $readmemh("rom/groups.mem",group_table);$readmemh("rom/sinc8_q18.mem",sinc_table);
  $readmemh("rom/cubic4_q18.mem",cubic_table);$readmemh("rom/group_recip_q24.mem",inverse_count);
 end
 reg [3:0] state;
 reg sinc_mode,all_mode;
 reg [6:0] group_index,group_count;
 reg [3:0] tap;
 reg [10:0] fraction_index;
 reg signed [31:0] lag_q15;
 reg signed [55:0] tap_sum;
 reg signed [43:0] score_sum;
 reg signed [20:0] coefficient;
 reg signed [69:0] average_product;
 reg signed [55:0] lag_product;
 reg signed [31:0] tap_value;
 reg signed [52:0] tap_product;
 wire vec_done,vec_busy;
 wire vec_start=start&&state==0;
 robust_direction_vector u_vector(clk,rst_n,vec_start,azimuth_q16,elevation_q16,vec_busy,vec_done,direction_x_q23,direction_y_q23,direction_z_q23);
 wire signed [4:0] dx=group_table[group_index][10:6];
 wire signed [4:0] dy=group_table[group_index][5:1];
 wire signed [31:0] delta_x=dx*direction_x_q23;
 wire signed [31:0] delta_y=dy*direction_y_q23;
 wire signed [32:0] spatial_delta=$signed(delta_x)+$signed(delta_y);
 wire group_selected=valid_groups[group_index]&&(all_mode||group_table[group_index][0]);
 wire signed [31:0] integer_lag=lag_q15>>>15;
 wire signed [31:0] sample_lag=integer_lag+$signed({1'b0,tap})-(sinc_mode?32'sd3:32'sd1);
 assign gcc_read_address={group_index,sample_lag[7:0]};
 wire signed [55:0] tap_sum_next=tap_sum+$signed(tap_product);
 always @(posedge clk) begin
  if(!rst_n) begin state<=0;busy<=0;done<=0;score_q23<=0;group_index<=0;group_count<=0;tap<=0;fraction_index<=0;lag_q15<=0;tap_sum<=0;score_sum<=0;sinc_mode<=0;all_mode<=0;end
  else begin
   done<=0;
   case(state)
    0: if(start) begin sinc_mode<=use_sinc;all_mode<=use_all_groups;score_sum<=0;group_count<=0;busy<=1;state<=1;end
    1: if(vec_done) begin group_index<=0;state<=2;end
    2: if(group_selected) begin lag_product<=spatial_delta*24'sd229280;state<=3;end
       else if(group_index==81)state<=9;else group_index<=group_index+1;
    3: begin lag_q15<=lag_product>>>23;tap<=0;tap_sum<=0;state<=4;end
    4: begin fraction_index<=({1'b0,lag_q15[14:0]}+16)>>5;state<=5;end
    5: begin coefficient<=sinc_mode?sinc_table[fraction_index*8+tap]:cubic_table[fraction_index*4+tap];state<=6;end
    6:begin tap_value<=gcc_read_value;state<=11;end
    11:begin tap_product<=tap_value*coefficient;state<=7;end
    7: begin
     if(tap==(sinc_mode?7:3)) begin
      score_sum<=score_sum+(tap_sum_next>>>18);group_count<=group_count+1;
      if(group_index==81)state<=9;else begin group_index<=group_index+1;state<=2;end
     end else begin tap_sum<=tap_sum_next;tap<=tap+1;state<=5;end
    end
    9: begin average_product<=score_sum*$signed({1'b0,inverse_count[group_count]});state<=10;end
    10: begin score_q23<=average_product>>>24;busy<=0;done<=1;state<=0;end
    default: begin state<=0;busy<=0;end
   endcase
  end
 end
endmodule
