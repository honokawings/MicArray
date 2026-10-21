`timescale 1ns/1ps
// Convert signed Q45 sum-of-48 samples to IEEE754 binary32.  The constant
// divide by48 is bit-serial (7-bit subtractor), never a long combinational
// divider. Absolute quotient truncation is <= 2^-45 before float rounding.
module denoise_fixed_to_float (
    input wire clk, input wire rst_n,
    input wire start, input wire signed [55:0] value,
    output wire busy, output reg valid, output reg [31:0] result
);
    localparam IDLE=0, DIVIDE=1, NORMALIZE=2, ROUND=3;
    reg [1:0] state;
    reg sign_bit;
    reg [55:0] dividend, quotient, normalized;
    reg [6:0] remainder;
    reg [5:0] remaining;
    reg signed [9:0] exponent;
    wire [6:0] trial = {remainder[5:0],dividend[55]};
    wire take_bit = trial >= 7'd48;
    wire [55:0] next_quotient = {quotient[54:0],take_bit};
    wire round_up = normalized[31] && ((|normalized[30:0]) || normalized[32]);
    wire [24:0] rounded = {1'b0,normalized[55:32]} + round_up;
    wire [9:0] biased_exp = exponent + 10'd127;
    wire [9:0] biased_exp_carry = exponent + 10'd128;
    assign busy = state != IDLE;
    always @(posedge clk) begin
        if (!rst_n) begin
            state<=IDLE; valid<=0; result<=0; sign_bit<=0;
            dividend<=0; quotient<=0; normalized<=0; remainder<=0;
            remaining<=0; exponent<=0;
        end else begin
            valid<=0;
            case(state)
            IDLE: if(start) begin
                sign_bit<=value[55];
                dividend<=value[55] ? (~value+56'd1) : value;
                quotient<=0; remainder<=0; remaining<=56;
                state<=DIVIDE;
            end
            DIVIDE: begin
                dividend<={dividend[54:0],1'b0};
                quotient<=next_quotient;
                remainder<=take_bit ? trial-7'd48 : trial;
                remaining<=remaining-1'b1;
                if(remaining==1) begin
                    normalized<=next_quotient; exponent<=10'sd10;
                    state<=NORMALIZE;
                end
            end
            NORMALIZE: begin
                if(normalized==0) begin
                    result<=0; valid<=1; state<=IDLE;
                end else if(normalized[55]) state<=ROUND;
                else begin normalized<=normalized<<1; exponent<=exponent-1'b1; end
            end
            ROUND: begin
                if(rounded[24]) result<={sign_bit,biased_exp_carry[7:0],rounded[23:1]};
                else result<={sign_bit,biased_exp[7:0],rounded[22:0]};
                valid<=1; state<=IDLE;
            end
            default: state<=IDLE;
            endcase
        end
    end
endmodule
