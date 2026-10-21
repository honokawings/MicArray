`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 2021/07/06 12:00:38
// Design Name: 
// Module Name: CalcMeanPower
// Project Name: 
// Target Devices: 
// Tool Versions: 
// Description: 
//  
// Dependencies: 
// 
// Revision:
// Revision 0.01 - File Created
// Additional Comments:
// 
//////////////////////////////////////////////////////////////////////////////////

 
module fpga_self_reset#(
	parameter Freq_Clock = 200E6,
	parameter MSecond_Delay = 100
)(
	input 	i_Clk				,
	input 	i_Locked_Clock		,
	input 	i_Rst_Inst			,
	
	output 	reg rst_global		,
	output 	reg rstn_global		,
	output 	reg rst_soft		,
	output 	reg rstn_soft		
    );
	
function integer clogb2(input integer bit_depth);
begin
	for(clogb2 = 0;bit_depth > 0;clogb2 = clogb2 + 1)
		bit_depth = bit_depth>>1;
end
endfunction	


parameter NDelay = Freq_Clock*MSecond_Delay/1000;
parameter MDelay = clogb2(NDelay-1);
	
reg [MDelay:0] cnt_Global;
always@(posedge i_Clk)begin
	if(!i_Locked_Clock)cnt_Global <= {1<<MDelay}-NDelay;
	else if(!cnt_Global[MDelay])cnt_Global <= cnt_Global + 1;
	
	rst_global	<= ~cnt_Global[MDelay];
	rstn_global	<= cnt_Global[MDelay];
end


reg 		rstS;
reg [MDelay:0] cnt_Soft;
always@(posedge i_Clk)begin
	rstS <= rst_global|i_Rst_Inst;

	if(rstS)cnt_Soft <=  {1<<MDelay}-NDelay;
	else if(~cnt_Soft[MDelay])cnt_Soft <= cnt_Soft + 1;
	
	rst_soft	<= ~cnt_Soft[MDelay];
	rstn_soft	<= cnt_Soft[MDelay];
end
	
endmodule
