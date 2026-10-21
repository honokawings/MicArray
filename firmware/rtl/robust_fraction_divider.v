`timescale 1ns/1ps
// Saturating UQ0.F fractional division. Exact unity cannot wrap to zero.
module robust_fraction_divider #(parameter W=64, parameter F=15)(
 input wire clk, rst_n, start,
 input wire [W-1:0] numerator, denominator,
 output reg busy, done,
 output reg [F-1:0] quotient
);
 reg [W:0] remainder, divisor;
 reg [F-1:0] work;
 reg [$clog2(F+1)-1:0] count;
 wire [W:0] shifted=remainder<<1;
 wire bit_value=shifted>=divisor;
 wire [W:0] reduced=bit_value?shifted-divisor:shifted;
 always @(posedge clk) begin
  if(!rst_n) begin busy<=0;done<=0;quotient<=0;work<=0;count<=0;remainder<=0;divisor<=0;end
  else begin
   done<=0;
   if(start&&!busy) begin
    if(denominator==0 || numerator==0) begin quotient<=0;done<=1;end
    else if(numerator>=denominator) begin quotient<={F{1'b1}};done<=1;end
    else begin remainder<={1'b0,numerator};divisor<={1'b0,denominator};work<=0;count<=0;busy<=1;end
   end else if(busy) begin
    remainder<=reduced;work<={work[F-2:0],bit_value};
    if(count==F-1) begin quotient<={work[F-2:0],bit_value};busy<=0;done<=1;end
    else count<=count+1'b1;
   end
  end
 end
endmodule
