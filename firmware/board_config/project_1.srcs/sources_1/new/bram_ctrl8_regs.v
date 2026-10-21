`timescale 1ns / 1ps

module bram_ctrl8_regs #(
    parameter ADDR_WIDTH = 13,
    parameter CTRL_BASE_ADDR = 0
)(
    (* X_INTERFACE_INFO = "xilinx.com:interface:bram:1.0 BRAM_PORTA CLK" *)
    input  wire                    bram_clk_a,

    (* X_INTERFACE_INFO = "xilinx.com:interface:bram:1.0 BRAM_PORTA RST" *)
    input  wire                    bram_rst_a,

    (* X_INTERFACE_INFO = "xilinx.com:interface:bram:1.0 BRAM_PORTA EN" *)
    input  wire                    bram_en_a,

    (* X_INTERFACE_INFO = "xilinx.com:interface:bram:1.0 BRAM_PORTA WE" *)
    input  wire [3:0]              bram_we_a,

    (* X_INTERFACE_INFO = "xilinx.com:interface:bram:1.0 BRAM_PORTA ADDR" *)
    input  wire [ADDR_WIDTH-1:0]   bram_addr_a,

    (* X_INTERFACE_INFO = "xilinx.com:interface:bram:1.0 BRAM_PORTA DIN" *)
    input  wire [31:0]             bram_wrdata_a,

    (* X_INTERFACE_INFO = "xilinx.com:interface:bram:1.0 BRAM_PORTA DOUT" *)
    output reg  [31:0]             bram_rddata_a,

    output reg signed [15:0]       ctrl0,
    output reg signed [15:0]       ctrl1,
    output reg signed [15:0]       ctrl2,
    output reg signed [15:0]       ctrl3,
    output reg signed [15:0]       ctrl4,
    output reg signed [15:0]       ctrl5,
    output reg signed [15:0]       ctrl6,
    output reg signed [15:0]       ctrl7,

    output reg [31:0]              word0,
    output reg [31:0]              word1,
    output reg [31:0]              word2,
    output reg [31:0]              word3
);

    wire hit_word0_byte = (bram_addr_a == CTRL_BASE_ADDR + 0);
    wire hit_word1_byte = (bram_addr_a == CTRL_BASE_ADDR + 4);
    wire hit_word2_byte = (bram_addr_a == CTRL_BASE_ADDR + 8);
    wire hit_word3_byte = (bram_addr_a == CTRL_BASE_ADDR + 12);

    wire hit_word0_word = (bram_addr_a == ((CTRL_BASE_ADDR >> 2) + 0));
    wire hit_word1_word = (bram_addr_a == ((CTRL_BASE_ADDR >> 2) + 1));
    wire hit_word2_word = (bram_addr_a == ((CTRL_BASE_ADDR >> 2) + 2));
    wire hit_word3_word = (bram_addr_a == ((CTRL_BASE_ADDR >> 2) + 3));

    wire hit_word0 = hit_word0_byte | hit_word0_word;
    wire hit_word1 = hit_word1_byte | hit_word1_word;
    wire hit_word2 = hit_word2_byte | hit_word2_word;
    wire hit_word3 = hit_word3_byte | hit_word3_word;

    wire write_en = bram_en_a && (bram_we_a != 4'b0000);

    function [31:0] apply_wstrb;
        input [31:0] old_data;
        input [31:0] new_data;
        input [3:0]  we;
        begin
            apply_wstrb = old_data;

            if (we[0])
                apply_wstrb[7:0] = new_data[7:0];

            if (we[1])
                apply_wstrb[15:8] = new_data[15:8];

            if (we[2])
                apply_wstrb[23:16] = new_data[23:16];

            if (we[3])
                apply_wstrb[31:24] = new_data[31:24];
        end
    endfunction

    always @(posedge bram_clk_a) begin
        if (bram_rst_a) begin
            word0 <= 32'd0;
            word1 <= 32'd0;
            word2 <= 32'd0;
            word3 <= 32'd0;
        end else begin
            if (write_en) begin
                if (hit_word0)
                    word0 <= apply_wstrb(word0, bram_wrdata_a, bram_we_a);

                if (hit_word1)
                    word1 <= apply_wstrb(word1, bram_wrdata_a, bram_we_a);

                if (hit_word2)
                    word2 <= apply_wstrb(word2, bram_wrdata_a, bram_we_a);

                if (hit_word3)
                    word3 <= apply_wstrb(word3, bram_wrdata_a, bram_we_a);
            end
        end
    end

    always @(posedge bram_clk_a) begin
        if (bram_rst_a) begin
            bram_rddata_a <= 32'd0;
        end else begin
            if (bram_en_a) begin
                if (hit_word0)
                    bram_rddata_a <= word0;
                else if (hit_word1)
                    bram_rddata_a <= word1;
                else if (hit_word2)
                    bram_rddata_a <= word2;
                else if (hit_word3)
                    bram_rddata_a <= word3;
                else
                    bram_rddata_a <= 32'd0;
            end
        end
    end

    always @(posedge bram_clk_a) begin
        if (bram_rst_a) begin
            ctrl0 <= 16'sd0;
            ctrl1 <= 16'sd0;
            ctrl2 <= 16'sd0;
            ctrl3 <= 16'sd0;
            ctrl4 <= 16'sd0;
            ctrl5 <= 16'sd0;
            ctrl6 <= 16'sd0;
            ctrl7 <= 16'sd0;
        end else begin
            ctrl0 <= word0[15:0];
            ctrl1 <= word0[31:16];

            ctrl2 <= word1[15:0];
            ctrl3 <= word1[31:16];

            ctrl4 <= word2[15:0];
            ctrl5 <= word2[31:16];

            ctrl6 <= word3[15:0];
            ctrl7 <= word3[31:16];
        end
    end

endmodule