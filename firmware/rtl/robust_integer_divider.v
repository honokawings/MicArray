`timescale 1ns/1ps
module robust_integer_divider #(parameter W=64)(
 input wire clk,rst_n,start,input wire [W-1:0] numerator,denominator,
 output reg busy,done,output reg [W-1:0] quotient
);
 reg [W-1:0] work,divisor;
 reg [W:0] remainder;
 reg [$clog2(W+1)-1:0] count;
 wire [W:0] shifted={remainder[W-1:0],work[W-1]};
 wire accept=shifted>={1'b0,divisor};
 always @(posedge clk)begin
  if(!rst_n)begin busy<=0;done<=0;quotient<=0;work<=0;divisor<=0;remainder<=0;count<=0;end
  else begin
   done<=0;
   if(start&&!busy)begin
    if(denominator==0)begin quotient<=0;done<=1;end
    else begin work<=numerator;divisor<=denominator;remainder<=0;count<=0;busy<=1;end
   end else if(busy)begin
    remainder<=accept?shifted-{1'b0,divisor}:shifted;work<={work[W-2:0],accept};
    if(count==W-1)begin quotient<={work[W-2:0],accept};busy<=0;done<=1;end else count<=count+1;
   end
  end
 end
endmodule
