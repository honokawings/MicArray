`timescale 1ns/1ps
// Restoring radix-four unsigned integer square root. floor(sqrt(radicand)).
module robust_isqrt #(parameter W=64)(
 input wire clk,rst_n,start,
 input wire [W-1:0] radicand,
 output reg busy,done,
 output reg [W/2-1:0] root
);
 localparam R=W/2;
 reg [W-1:0] bits_work;
 reg [W+1:0] remainder;
 reg [R-1:0] root_work;
 reg [$clog2(R+1)-1:0] count;
 wire [W+1:0] trial_remainder=(remainder<<2)|bits_work[W-1:W-2];
 wire [W+1:0] trial_divisor=({{(W+2-R){1'b0}},root_work}<<2)|{{(W+1){1'b0}},1'b1};
 wire accept=trial_remainder>=trial_divisor;
 always @(posedge clk) begin
  if(!rst_n) begin busy<=0;done<=0;root<=0;root_work<=0;remainder<=0;bits_work<=0;count<=0;end
  else begin
   done<=0;
   if(start&&!busy) begin bits_work<=radicand;root_work<=0;remainder<=0;count<=0;busy<=1;end
   else if(busy) begin
    bits_work<=bits_work<<2;
    remainder<=accept?trial_remainder-trial_divisor:trial_remainder;
    root_work<={root_work[R-2:0],accept};
    if(count==R-1) begin root<={root_work[R-2:0],accept};busy<=0;done<=1;end
    else count<=count+1'b1;
   end
  end
 end
endmodule
