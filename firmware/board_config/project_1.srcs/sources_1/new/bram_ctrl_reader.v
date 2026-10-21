`timescale 1ns / 1ps

module bram_ctrl_reader #(
    parameter ADDR_WIDTH     = 32,
    parameter ADDR_STEP      = 4,
    parameter CTRL_BASE_ADDR = 0
)(
    input  wire                     clk,
    input  wire                     rst_n,

    // =========================================================
    // BRAM Port B
    // =========================================================
    output wire                     bram_clk,
    output wire                     bram_rst,
    output wire                     bram_en,
    output wire [3:0]               bram_we,
    output wire [31:0]              bram_din,
    output reg  [ADDR_WIDTH-1:0]    bram_addr,
    input  wire [31:0]              bram_dout,

    // =========================================================
    // 8 ·������
    // =========================================================
    output reg signed [15:0]        ctrl0,
    output reg signed [15:0]        ctrl1,
    output reg signed [15:0]        ctrl2,
    output reg signed [15:0]        ctrl3,
    output reg signed [15:0]        ctrl4,
    output reg signed [15:0]        ctrl5,
    output reg signed [15:0]        ctrl6,
    output reg signed [15:0]        ctrl7,

    // =========================================================
    // ILA �����ź�
    // =========================================================
    output reg [31:0]               word0,
    output reg [31:0]               word1,
    output reg [31:0]               word2,
    output reg [31:0]               word3,
    output reg [2:0]                rd_state
);

    assign bram_clk = clk;
    assign bram_rst = ~rst_n;
    assign bram_en  = 1'b1;
    assign bram_we  = 4'b0000;
    assign bram_din = 32'd0;

    localparam [ADDR_WIDTH-1:0] ADDR0 = CTRL_BASE_ADDR + ADDR_STEP * 0;
    localparam [ADDR_WIDTH-1:0] ADDR1 = CTRL_BASE_ADDR + ADDR_STEP * 1;
    localparam [ADDR_WIDTH-1:0] ADDR2 = CTRL_BASE_ADDR + ADDR_STEP * 2;
    localparam [ADDR_WIDTH-1:0] ADDR3 = CTRL_BASE_ADDR + ADDR_STEP * 3;

    reg init_done;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rd_state  <= 3'd0;
            init_done <= 1'b0;
            bram_addr <= ADDR0;

            word0 <= 32'd0;
            word1 <= 32'd0;
            word2 <= 32'd0;
            word3 <= 32'd0;

            ctrl0 <= 16'sd0;
            ctrl1 <= 16'sd0;
            ctrl2 <= 16'sd0;
            ctrl3 <= 16'sd0;
            ctrl4 <= 16'sd0;
            ctrl5 <= 16'sd0;
            ctrl6 <= 16'sd0;
            ctrl7 <= 16'sd0;
        end else begin
            case (rd_state)

                3'd0: begin
                    if (init_done) begin
                        word3 <= bram_dout;
                        ctrl6 <= bram_dout[15:0];
                        ctrl7 <= bram_dout[31:16];
                    end

                    bram_addr <= ADDR0;
                    rd_state  <= 3'd1;
                end

                // �ȴ� BRAM ͬ����
                3'd1: begin
                    rd_state <= 3'd2;
                end

                // ���� ADDR0 ������
                3'd2: begin
                    word0 <= bram_dout;
                    ctrl0 <= bram_dout[15:0];
                    ctrl1 <= bram_dout[31:16];

                    bram_addr <= ADDR1;
                    rd_state  <= 3'd3;
                end

                // �ȴ� BRAM ͬ����
                3'd3: begin
                    rd_state <= 3'd4;
                end

                // ���� ADDR1 ������
                3'd4: begin
                    word1 <= bram_dout;
                    ctrl2 <= bram_dout[15:0];
                    ctrl3 <= bram_dout[31:16];

                    bram_addr <= ADDR2;
                    rd_state  <= 3'd5;
                end

                // �ȴ� BRAM ͬ����
                3'd5: begin
                    rd_state <= 3'd6;
                end

                // ���� ADDR2 ������
                3'd6: begin
                    word2 <= bram_dout;
                    ctrl4 <= bram_dout[15:0];
                    ctrl5 <= bram_dout[31:16];

                    bram_addr <= ADDR3;
                    rd_state  <= 3'd7;
                end

                // �ȴ� ADDR3 �����ݣ���һ�� state0 ����
                3'd7: begin
                    init_done <= 1'b1;
                    rd_state  <= 3'd0;
                end

                default: begin
                    rd_state  <= 3'd0;
                    bram_addr <= ADDR0;
                end
            endcase
        end
    end

endmodule