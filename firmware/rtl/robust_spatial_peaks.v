`timescale 1ns/1ps
// One full spatial scan is shared by multi-peak detection and the displayed map.
// Local maxima are separated by spherical angle; a single broad peak cannot
// become three sources merely because three output slots are available.
module robust_spatial_peaks (
 input wire clk,rst_n,start,
 output reg score_start,
 output wire score_use_sinc,score_all_groups,
 output reg signed [31:0] score_azimuth_q16,score_elevation_q16,
 input wire score_done,input wire signed [31:0] score_q23,
 input wire [1:0] seed_select,
 output wire signed [31:0] seed_azimuth_q16,seed_elevation_q16,seed_score_q23,
 output reg [1:0] seed_count,
 output wire signed [31:0] background_score_q23,
 output reg map_write_valid,output reg [10:0] map_write_index,
 output reg [7:0] map_write_value,
 output reg busy,done
);
 localparam IDLE=0,SCAN_REQUEST=1,SCAN_WAIT=2,SELECT_INIT=3,
  CENTER_REQUEST=4,CENTER_WAIT=5,CENTER_TEST=6,
  NEIGHBOR_REQUEST=7,NEIGHBOR_WAIT=8,NEIGHBOR_TEST=9,
  VECTOR_START=10,VECTOR_WAIT=11,VECTOR_TEST=12,NEXT_CENTER=13,
  BACKGROUND_INIT=14,BACKGROUND_REQUEST=15,BACKGROUND_WAIT=16,
  BACKGROUND_VECTOR_START=17,BACKGROUND_VECTOR_WAIT=18,BACKGROUND_TEST=19,
  NORMALIZE_INIT=20,NORMALIZE_REQUEST=21,NORMALIZE_WAIT=22,
  DIVIDE_START=23,DIVIDE_WAIT=24,FINISH=25;
 reg [4:0] state;
 (* ram_style="block" *) reg signed [31:0] responses[0:2047];
 reg [10:0] read_index,index,best_index;
 reg signed [31:0] read_value,center_value,best_score,global_max;
 reg [5:0] scan_column,scan_row,column,row;
 reg [3:0] neighbor;
 reg signed [31:0] seeds_az[0:2],seeds_el[0:2],seeds_score[0:2];
 reg signed [31:0] local_background[0:2],global_background;
 assign background_score_q23=seed_count>1?local_background[seed_select]:global_background;
 reg signed [24:0] seeds_x[0:2],seeds_y[0:2],seeds_z[0:2];
 reg signed [24:0] best_x,best_y,best_z;
 assign seed_azimuth_q16=seeds_az[seed_select];
 assign seed_elevation_q16=seeds_el[seed_select];
 assign seed_score_q23=seeds_score[seed_select];
 assign score_use_sinc=1'b0;
 assign score_all_groups=1'b1;
 always @(posedge clk) read_value<=responses[read_index];
 reg signed [6:0] neighbor_column,neighbor_row;
 always @* begin
  neighbor_column=$signed({1'b0,column});
  neighbor_row=$signed({1'b0,row});
  case(neighbor)
   0:begin neighbor_column=neighbor_column-1;neighbor_row=neighbor_row-1;end
   1:neighbor_row=neighbor_row-1;
   2:begin neighbor_column=neighbor_column+1;neighbor_row=neighbor_row-1;end
   3:neighbor_column=neighbor_column-1;
   4:neighbor_column=neighbor_column+1;
   5:begin neighbor_column=neighbor_column-1;neighbor_row=neighbor_row+1;end
   6:neighbor_row=neighbor_row+1;
   7:begin neighbor_column=neighbor_column+1;neighbor_row=neighbor_row+1;end
  endcase
 end
 wire neighbor_in_grid=neighbor_column>=0&&neighbor_column<37&&neighbor_row>=0&&neighbor_row<37;
 wire [10:0] neighbor_index=neighbor_row*37+neighbor_column;
 wire signed [31:0] current_az=-32'sd5898240+$signed({1'b0,column})*32'sd327680;
 wire signed [31:0] current_el=-32'sd5898240+$signed({1'b0,row})*32'sd327680;
 wire vector_start=state==VECTOR_START||state==BACKGROUND_VECTOR_START;
 wire vector_done,vector_busy;
 wire signed [24:0] vx,vy,vz;
 robust_direction_vector u_vector(clk,rst_n,vector_start,current_az,current_el,vector_busy,vector_done,vx,vy,vz);
 wire signed [51:0] dot0=vx*seeds_x[0]+vy*seeds_y[0]+vz*seeds_z[0];
 wire signed [51:0] dot1=vx*seeds_x[1]+vy*seeds_y[1]+vz*seeds_z[1];
 wire signed [51:0] dot2=vx*seeds_x[2]+vy*seeds_y[2]+vz*seeds_z[2];
 // cos(12 degrees) and cos(5 degrees), Q46.
 localparam signed [51:0] COS12_Q46=52'sd68831018284033;
 localparam signed [51:0] COS5_Q46=52'sd70100969861163;
 wire too_close=(seed_count>0&&dot0>COS12_Q46)||(seed_count>1&&dot1>COS12_Q46);
 wire outside_peaks=(seed_count==0||dot0<=COS5_Q46)&&
                    (seed_count<2||dot1<=COS5_Q46)&&
                    (seed_count<3||dot2<=COS5_Q46);
 wire [39:0] norm_numerator={8'd0,$unsigned(read_value)}*8'd255;
 wire div_done,div_busy;wire [39:0] norm_quotient;
 robust_integer_divider #(.W(40)) u_normalize(
  clk,rst_n,state==DIVIDE_START,norm_numerator,
  {8'd0,$unsigned(global_max)},div_busy,div_done,norm_quotient);
 integer k;
 always @(posedge clk) begin
  if(!rst_n) begin
   state<=IDLE;busy<=0;done<=0;score_start<=0;seed_count<=0;
   score_azimuth_q16<=0;score_elevation_q16<=0;global_background<=0;
   map_write_valid<=0;map_write_index<=0;map_write_value<=0;
   index<=0;read_index<=0;scan_column<=0;scan_row<=0;column<=0;row<=0;
   best_score<=0;global_max<=0;center_value<=0;neighbor<=0;best_index<=0;
   best_x<=0;best_y<=0;best_z<=0;
   for(k=0;k<3;k=k+1)begin seeds_az[k]<=0;seeds_el[k]<=0;seeds_score[k]<=0;seeds_x[k]<=0;seeds_y[k]<=0;seeds_z[k]<=0;local_background[k]<=0;end
  end else begin
   done<=0;score_start<=0;map_write_valid<=0;
   case(state)
    IDLE:if(start)begin busy<=1;seed_count<=0;global_max<=0;scan_column<=0;scan_row<=0;index<=0;state<=SCAN_REQUEST;end
    SCAN_REQUEST:begin
     score_azimuth_q16<=-32'sd5898240+$signed({1'b0,scan_column})*32'sd327680;
     score_elevation_q16<=-32'sd5898240+$signed({1'b0,scan_row})*32'sd327680;
     score_start<=1;state<=SCAN_WAIT;
    end
    SCAN_WAIT:if(score_done)begin
     responses[index]<=score_q23;
     if(score_q23>global_max)global_max<=score_q23;
     if(index==1368)state<=SELECT_INIT;
     else begin index<=index+1;scan_column<=scan_column==36?0:scan_column+1;if(scan_column==36)scan_row<=scan_row+1;state<=SCAN_REQUEST;end
    end
    SELECT_INIT:begin index<=0;column<=0;row<=0;best_score<=0;best_index<=0;state<=CENTER_REQUEST;end
    CENTER_REQUEST:begin read_index<=index;state<=CENTER_WAIT;end
    CENTER_WAIT:state<=CENTER_TEST;
    CENTER_TEST:begin
     center_value<=read_value;neighbor<=0;
     // A weak candidate is not promoted simply by frame-wise colour scaling.
     if(read_value<=best_score||read_value<=0||
        $signed(read_value)*64'sd100<$signed(global_max)*64'sd20)state<=NEXT_CENTER;
     else state<=NEIGHBOR_REQUEST;
    end
    NEIGHBOR_REQUEST:begin
     if(!neighbor_in_grid)begin if(neighbor==7)state<=VECTOR_START;else neighbor<=neighbor+1;end
     else begin read_index<=neighbor_index;state<=NEIGHBOR_WAIT;end
    end
    NEIGHBOR_WAIT:state<=NEIGHBOR_TEST;
    NEIGHBOR_TEST:begin
     if(read_value>center_value||(read_value==center_value&&read_index<index))state<=NEXT_CENTER;
     else if(neighbor==7)state<=VECTOR_START;
     else begin neighbor<=neighbor+1;state<=NEIGHBOR_REQUEST;end
    end
    VECTOR_START:state<=VECTOR_WAIT;
    VECTOR_WAIT:if(vector_done)state<=VECTOR_TEST;
    VECTOR_TEST:begin
     if(!too_close)begin best_score<=center_value;best_index<=index;best_x<=vx;best_y<=vy;best_z<=vz;end
     state<=NEXT_CENTER;
    end
    NEXT_CENTER:begin
     if(index==1368)begin
      if(best_score>0)begin
       seeds_az[seed_count]<=-32'sd5898240+$signed({1'b0,best_index%37})*32'sd327680;
       seeds_el[seed_count]<=-32'sd5898240+$signed({1'b0,best_index/37})*32'sd327680;
       seeds_score[seed_count]<=best_score;seeds_x[seed_count]<=best_x;seeds_y[seed_count]<=best_y;seeds_z[seed_count]<=best_z;
       seed_count<=seed_count+1;
       state<=seed_count==2?BACKGROUND_INIT:SELECT_INIT;
      end else state<=BACKGROUND_INIT;
     end else begin index<=index+1;column<=column==36?0:column+1;if(column==36)row<=row+1;state<=CENTER_REQUEST;end
    end
    BACKGROUND_INIT:begin index<=0;column<=0;row<=0;global_background<=0;for(k=0;k<3;k=k+1)local_background[k]<=0;state<=BACKGROUND_REQUEST;end
    BACKGROUND_REQUEST:begin read_index<=index;state<=BACKGROUND_WAIT;end
    BACKGROUND_WAIT:state<=BACKGROUND_VECTOR_START;
    BACKGROUND_VECTOR_START:begin center_value<=read_value;state<=BACKGROUND_VECTOR_WAIT;end
    BACKGROUND_VECTOR_WAIT:if(vector_done)state<=BACKGROUND_TEST;
    BACKGROUND_TEST:begin
     if(outside_peaks)begin
      if(center_value>global_background)global_background<=center_value;
      // Multi-source contrast is local. The tail of a stronger, distant source
      // must not be used as the noise floor of every weaker source.
      if(seed_count>0&&dot0<=COS5_Q46&&dot0>=COS12_Q46&&center_value>local_background[0])local_background[0]<=center_value;
      if(seed_count>1&&dot1<=COS5_Q46&&dot1>=COS12_Q46&&center_value>local_background[1])local_background[1]<=center_value;
      if(seed_count>2&&dot2<=COS5_Q46&&dot2>=COS12_Q46&&center_value>local_background[2])local_background[2]<=center_value;
     end
     if(index==1368)state<=NORMALIZE_INIT;
     else begin index<=index+1;column<=column==36?0:column+1;if(column==36)row<=row+1;state<=BACKGROUND_REQUEST;end
    end
    NORMALIZE_INIT:begin index<=0;state<=NORMALIZE_REQUEST;end
    NORMALIZE_REQUEST:begin read_index<=index;state<=NORMALIZE_WAIT;end
    NORMALIZE_WAIT:state<=DIVIDE_START;
    DIVIDE_START:begin
     if(read_value<=0||global_max<=0)begin
      map_write_valid<=1;map_write_index<=index;map_write_value<=0;
      if(index==1368)state<=FINISH;else begin index<=index+1;state<=NORMALIZE_REQUEST;end
     end else state<=DIVIDE_WAIT;
    end
    DIVIDE_WAIT:if(div_done)begin
     map_write_valid<=1;map_write_index<=index;map_write_value<=norm_quotient>255?255:norm_quotient[7:0];
     if(index==1368)state<=FINISH;else begin index<=index+1;state<=NORMALIZE_REQUEST;end
    end
    FINISH:begin done<=1;busy<=0;state<=IDLE;end
    default:begin state<=IDLE;busy<=0;seed_count<=0;end
   endcase
  end
 end
endmodule
