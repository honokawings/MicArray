`timescale 1ns / 1ps

module ICS43432_48CH_SERIAL (
    input  wire        clk_6_144MHz,
    input  wire        rst_n,

    output wire [23:0] SCK,
    output wire [23:0] WS,
    input  wire [47:0] SD,

    output wire        clk_48k,

    output wire signed [23:0] ch1,
    output wire signed [23:0] ch2,
    output wire signed [23:0] ch3,
    output wire signed [23:0] ch4,
    output wire signed [23:0] ch5,
    output wire signed [23:0] ch6,
    output wire signed [23:0] ch7,
    output wire signed [23:0] ch8,
    output wire signed [23:0] ch9,
    output wire signed [23:0] ch10,
    output wire signed [23:0] ch11,
    output wire signed [23:0] ch12,
    output wire signed [23:0] ch13,
    output wire signed [23:0] ch14,
    output wire signed [23:0] ch15,
    output wire signed [23:0] ch16,
    output wire signed [23:0] ch17,
    output wire signed [23:0] ch18,
    output wire signed [23:0] ch19,
    output wire signed [23:0] ch20,
    output wire signed [23:0] ch21,
    output wire signed [23:0] ch22,
    output wire signed [23:0] ch23,
    output wire signed [23:0] ch24,
    output wire signed [23:0] ch25,
    output wire signed [23:0] ch26,
    output wire signed [23:0] ch27,
    output wire signed [23:0] ch28,
    output wire signed [23:0] ch29,
    output wire signed [23:0] ch30,
    output wire signed [23:0] ch31,
    output wire signed [23:0] ch32,
    output wire signed [23:0] ch33,
    output wire signed [23:0] ch34,
    output wire signed [23:0] ch35,
    output wire signed [23:0] ch36,
    output wire signed [23:0] ch37,
    output wire signed [23:0] ch38,
    output wire signed [23:0] ch39,
    output wire signed [23:0] ch40,
    output wire signed [23:0] ch41,
    output wire signed [23:0] ch42,
    output wire signed [23:0] ch43,
    output wire signed [23:0] ch44,
    output wire signed [23:0] ch45,
    output wire signed [23:0] ch46,
    output wire signed [23:0] ch47,
    output wire signed [23:0] ch48
);

    reg sck_r;
    reg [5:0] bit_cnt;

    reg [23:0] shift_reg [0:47];
    reg signed [23:0] audio_reg [0:47];

    integer i;

    assign SCK = {24{sck_r}};
    assign WS  = {24{bit_cnt[5]}};

    BUFG u_bufg_clk_48k (
        .I(bit_cnt[5]),
        .O(clk_48k)
    );

    always @(posedge clk_6_144MHz or negedge rst_n) begin
        if (!rst_n) begin
            sck_r   <= 1'b0;
            bit_cnt <= 6'd0;

            for (i = 0; i < 48; i = i + 1) begin
                shift_reg[i] <= 24'd0;
                audio_reg[i] <= 24'sd0;
            end
        end
        else begin
            sck_r <= ~sck_r;

            if (!sck_r) begin
                bit_cnt <= bit_cnt + 1'b1;

                if ((bit_cnt >= 6'd1) && (bit_cnt <= 6'd24)) begin
                    for (i = 0; i < 48; i = i + 1)
                        shift_reg[i] <= {shift_reg[i][22:0], SD[i]};
                end

                if (bit_cnt == 6'd28) begin
                    for (i = 0; i < 48; i = i + 1)
                        audio_reg[i] <= shift_reg[i];
                end
            end
        end
    end

    assign {
        ch48, ch47, ch46, ch45, ch44, ch43, ch42, ch41,
        ch40, ch39, ch38, ch37, ch36, ch35, ch34, ch33,
        ch32, ch31, ch30, ch29, ch28, ch27, ch26, ch25,
        ch24, ch23, ch22, ch21, ch20, ch19, ch18, ch17,
        ch16, ch15, ch14, ch13, ch12, ch11, ch10, ch9,
        ch8, ch7, ch6, ch5, ch4, ch3, ch2, ch1
    } = {
        audio_reg[47], audio_reg[46], audio_reg[45], audio_reg[44],
        audio_reg[43], audio_reg[42], audio_reg[41], audio_reg[40],
        audio_reg[39], audio_reg[38], audio_reg[37], audio_reg[36],
        audio_reg[35], audio_reg[34], audio_reg[33], audio_reg[32],
        audio_reg[31], audio_reg[30], audio_reg[29], audio_reg[28],
        audio_reg[27], audio_reg[26], audio_reg[25], audio_reg[24],
        audio_reg[23], audio_reg[22], audio_reg[21], audio_reg[20],
        audio_reg[19], audio_reg[18], audio_reg[17], audio_reg[16],
        audio_reg[15], audio_reg[14], audio_reg[13], audio_reg[12],
        audio_reg[11], audio_reg[10], audio_reg[9],  audio_reg[8],
        audio_reg[7],  audio_reg[6],  audio_reg[5],  audio_reg[4],
        audio_reg[3],  audio_reg[2],  audio_reg[1],  audio_reg[0]
    };

endmodule