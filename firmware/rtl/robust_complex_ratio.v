`timescale 1ns/1ps
// Independently latch both divider completions, including zero/unity fast paths.
module robust_complex_ratio(
 input wire clk,rst_n,start,
 input wire signed [63:0] real_numerator,imag_numerator,
 input wire [63:0] denominator,
 output reg busy,done,
 output reg signed [15:0] real_q15,imag_q15
);
 wire [63:0] nr=real_numerator[63]?-real_numerator:real_numerator;
 wire [63:0] ni=imag_numerator[63]?-imag_numerator:imag_numerator;
 wire [14:0] qr,qi;
 wire br,bi,dr,di;
 reg sr,si,seen_r,seen_i;
 robust_fraction_divider u_real(clk,rst_n,start&&!busy,nr,denominator,br,dr,qr);
 robust_fraction_divider u_imag(clk,rst_n,start&&!busy,ni,denominator,bi,di,qi);
 always @(posedge clk) begin
  if(!rst_n)begin busy<=0;done<=0;real_q15<=0;imag_q15<=0;seen_r<=0;seen_i<=0;sr<=0;si<=0;end
  else begin
   done<=0;
   if(start&&!busy)begin busy<=1;sr<=real_numerator[63];si<=imag_numerator[63];seen_r<=0;seen_i<=0;end
   else if(busy)begin
    if(dr)begin seen_r<=1;real_q15<=sr?-$signed({1'b0,qr}):$signed({1'b0,qr});end
    if(di)begin seen_i<=1;imag_q15<=si?-$signed({1'b0,qi}):$signed({1'b0,qi});end
    if((seen_r||dr)&&(seen_i||di))begin busy<=0;done<=1;seen_r<=0;seen_i<=0;end
   end
  end
 end
endmodule
