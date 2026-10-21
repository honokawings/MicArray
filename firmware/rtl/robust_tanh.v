`timescale 1ns/1ps
module robust_tanh(input wire clk,rst_n,start,input wire signed [31:0] value_q16,output reg busy,done,output reg signed [31:0] result_q16);
 (* rom_style="block" *)reg [16:0] table_values[0:8192];
 initial $readmemh("rom/nn_tanh_q16.mem",table_values);
 reg [1:0] state;reg negative;reg [13:0] index;reg [5:0] fraction;
 reg [16:0] low,high;
 wire [31:0] absolute=value_q16[31]?-value_q16:value_q16;
 wire signed [17:0] delta=$signed({1'b0,high})-$signed({1'b0,low});
 wire signed [24:0] interpolation=delta*$signed({1'b0,fraction});
 wire signed [31:0] positive=$signed({1'b0,low})+(interpolation>>>6);
 always @(posedge clk)begin
  if(!rst_n)begin state<=0;busy<=0;done<=0;result_q16<=0;index<=0;fraction<=0;negative<=0;end
  else begin done<=0;case(state)
   0:if(start)begin negative<=value_q16[31];index<=absolute>=524288?14'd8192:absolute[19:6];fraction<=absolute>=524288?0:absolute[5:0];busy<=1;state<=1;end
   1:begin low<=table_values[index];high<=table_values[index==8192?8192:index+1];state<=2;end
   2:begin result_q16<=negative?-positive:positive;busy<=0;done<=1;state<=0;end
   default:begin state<=0;busy<=0;end
  endcase end
 end
endmodule
