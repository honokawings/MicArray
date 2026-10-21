`timescale 1ns / 1ps

module Mic_Frame_Packer #(
    parameter DATA_WIDTH   = 128,
    parameter AUDIO_FRAMES = 200,
    parameter CHANNELS     = 48,
    parameter [15:0] FRAME_HEAD     = 16'hAABB,
    parameter [15:0] FRAME_SEQUENCE = 16'hEEFF
)(
    input  wire                    clk,
    input  wire                    rst_n,

    input  wire [31:0]             mic_data_32,
    input  wire                    mic_data_valid,
    output wire                    mic_data_ready,
    input  wire [5:0]              mic_channel_index,

    output reg  [DATA_WIDTH-1:0]   m_axis_tdata,
    output wire [DATA_WIDTH/8-1:0] m_axis_tkeep,
    output reg                     m_axis_tlast,
    output reg                     m_axis_tvalid,
    input  wire                    m_axis_tready
);

    localparam integer BYTE_PER_BEAT = DATA_WIDTH / 8;
    localparam integer PAYLOAD_WORDS = AUDIO_FRAMES * CHANNELS;
    localparam [31:0] PAYLOAD_BYTES  = PAYLOAD_WORDS * 4;

    localparam [1:0] IDLE    = 2'd0;
    localparam [1:0] HEADER  = 2'd1;
    localparam [1:0] PAYLOAD = 2'd2;

    reg [1:0]  state;
    reg [1:0]  pack_word_sel;
    reg [95:0] pack_reg_96;
    reg [31:0] payload_word_count;
    reg [31:0] packet_seq;

    wire mic_fire;
    wire axis_fire;

    assign m_axis_tkeep = {BYTE_PER_BEAT{1'b1}};

    assign mic_data_ready =
        (state == IDLE) ||
        ((state == PAYLOAD) && !m_axis_tvalid);

    assign mic_fire  = mic_data_valid && mic_data_ready;
    assign axis_fire = m_axis_tvalid && m_axis_tready;

    function [DATA_WIDTH-1:0] make_header;
        input [31:0] seq;
        begin
            make_header = {
                32'hFFFFFFFF,
                seq,
                PAYLOAD_BYTES,
                FRAME_SEQUENCE,
                FRAME_HEAD
            };
        end
    endfunction

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state              <= IDLE;
            pack_word_sel      <= 2'd0;
            pack_reg_96        <= 96'd0;
            payload_word_count <= 32'd0;
            packet_seq         <= 32'd1;

            m_axis_tdata       <= {DATA_WIDTH{1'b0}};
            m_axis_tlast       <= 1'b0;
            m_axis_tvalid      <= 1'b0;
        end
        else begin

            if (axis_fire) begin
                if (state == HEADER) begin
                    m_axis_tvalid <= 1'b0;
                    state         <= PAYLOAD;
                end
                else if (state == PAYLOAD) begin
                    m_axis_tvalid <= 1'b0;
                    m_axis_tlast  <= 1'b0;

                    if (m_axis_tlast) begin
                        state              <= IDLE;
                        pack_word_sel      <= 2'd0;
                        payload_word_count <= 32'd0;

                        packet_seq <=
                            (packet_seq == 32'hFFFFFFFF)
                            ? 32'd1
                            : packet_seq + 1'b1;
                    end
                end
            end

            if (mic_fire) begin
                if (state == IDLE) begin
                    if (mic_channel_index == 6'd0) begin
                        pack_reg_96        <= 96'd0;
                        pack_reg_96[31:0]  <= mic_data_32;
                        pack_word_sel      <= 2'd1;
                        payload_word_count <= 32'd1;

                        m_axis_tdata       <= make_header(packet_seq);
                        m_axis_tlast       <= 1'b0;
                        m_axis_tvalid      <= 1'b1;

                        state              <= HEADER;
                    end
                end
                else if (state == PAYLOAD) begin
                    case (pack_word_sel)

                        2'd0: begin
                            pack_reg_96[31:0] <= mic_data_32;
                            pack_word_sel     <= 2'd1;
                        end

                        2'd1: begin
                            pack_reg_96[63:32] <= mic_data_32;
                            pack_word_sel      <= 2'd2;
                        end

                        2'd2: begin
                            pack_reg_96[95:64] <= mic_data_32;
                            pack_word_sel      <= 2'd3;
                        end

                        2'd3: begin
                            m_axis_tdata  <= {mic_data_32, pack_reg_96};
                            m_axis_tlast  <=
                                (payload_word_count == PAYLOAD_WORDS - 1);
                            m_axis_tvalid <= 1'b1;
                            pack_word_sel <= 2'd0;
                        end

                    endcase

                    if (payload_word_count != PAYLOAD_WORDS - 1)
                        payload_word_count <= payload_word_count + 1'b1;
                end
            end
        end
    end

endmodule